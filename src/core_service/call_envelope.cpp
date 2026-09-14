#include "call_envelope.h"

namespace core_service {
namespace {

// The CLOSED SET of provider-refusal codes, in one place so it cannot drift
// against the rest of the function. See call_envelope.h for why the set is
// closed and why "unknown_method" is listed before anything emits it.
const char* const kRejectionCodes[] = {
    "dispatch_failed", "invalid_args", "unknown_method",
};

bool isRejectionCode(const std::string& c)
{
    for (const char* k : kRejectionCodes)
        if (c == k) return true;
    return false;
}

// What a declared parameter type accepts, as a JSON predicate.
//
// Only the types whose JSON counterpart is UNAMBIGUOUS appear here. "QVariant"
// is deliberately absent: it is what `any` and every `?T` publish as, and it
// accepts anything. So is "QByteArray" -- `bstr` rides a string OR the
// canonical {"_bytes": ...} tag, so two shapes satisfy it and a third
// (a number) is refused by the provider with the same sentence this would use.
//
// These are the QT spelling, which is what a handcrafted Qt plugin (from its
// QMetaObject) and logos-rust-sdk's provider publish. The C++ cdylib backend
// publishes LIDL CONTRACT names instead (lidl_gen_cdylib's
// lidlTypeToPublishedName), and only its primitives -- "int", "uint", "bool" --
// happen to collide with words above; "tstr", "bstr", "float64", "[T]",
// "{K: V}" and "? T" are absent, so a cdylib module's string and container
// parameters fall through to silence, the same answer any unrecognised name
// gets. Teaching the table that half is a BEHAVIOUR change rather than a lookup
// change -- calls that answer {"result": null, "status": "ok"} today would
// start failing with exit code 4 -- so it wants its own integration case, not a
// line added here. (The collisions are harmless where they are: `int` is 32-bit
// in one vocabulary and 64-bit in the other, which matters to a reader that
// reasons about RANGE and not to predicates that only separate a number from a
// non-number. See plugin_introspect.cpp for what that ambiguity cost there.)
struct DeclaredType {
    const char* qtName;
    const char* expected;                       // the word in the message
    bool (*accepts)(const nlohmann::json& v);
};

const DeclaredType kDeclaredTypes[] = {
    {"QString",      "string",  [](const nlohmann::json& v) { return v.is_string(); }},
    {"bool",         "bool",    [](const nlohmann::json& v) { return v.is_boolean(); }},
    // `uint` publishes as "int" too (logos-rust-sdk's qt_type_name), so the
    // signedness is the provider's to check; this only separates a number from
    // everything that is not one. A FRACTIONAL number is left alone for the
    // same reason -- the provider owns the whole-number rule and states it.
    {"int",          "integer", [](const nlohmann::json& v) { return v.is_number(); }},
    {"uint",         "integer", [](const nlohmann::json& v) { return v.is_number(); }},
    {"qlonglong",    "integer", [](const nlohmann::json& v) { return v.is_number(); }},
    {"qulonglong",   "integer", [](const nlohmann::json& v) { return v.is_number(); }},
    {"double",       "number",  [](const nlohmann::json& v) { return v.is_number(); }},
    {"float",        "number",  [](const nlohmann::json& v) { return v.is_number(); }},
    {"QVariantList", "array",   [](const nlohmann::json& v) { return v.is_array(); }},
    {"QStringList",  "array",   [](const nlohmann::json& v) { return v.is_array(); }},
    {"QVariantMap",  "object",  [](const nlohmann::json& v) { return v.is_object(); }},
};

const DeclaredType* declaredType(const std::string& qtName)
{
    for (const DeclaredType& t : kDeclaredTypes)
        if (qtName == t.qtName) return &t;
    return nullptr;
}

} // namespace

bool argumentMismatch(const MethodInfo& method,
                      const nlohmann::json& args,
                      std::string& reason)
{
    if (!method.paramsPublished) return false;
    if (!args.is_array()) return false;
    // A COUNT is class B, and the provider answers it with invalid_args. Saying
    // anything here would be guessing at a call the provider already judges.
    if (args.size() != method.paramTypes.size()) return false;

    for (size_t i = 0; i < method.paramTypes.size(); ++i) {
        const nlohmann::json& v = args[i];
        if (v.is_null()) continue;   // `?T`'s empty state; no declared type refuses it here
        const DeclaredType* t = declaredType(method.paramTypes[i]);
        if (!t || t->accepts(v)) continue;
        reason = std::string("expected ") + t->expected + " at arg" + std::to_string(i)
               + ", got " + v.type_name();
        return true;
    }
    return false;
}

bool dispatchRejection(const nlohmann::json& v, CallFailure& out)
{
    if (!v.is_object() || v.size() != 3) return false;
    auto code = v.find("code"), message = v.find("message"), origin = v.find("origin");
    if (code == v.end() || message == v.end() || origin == v.end()) return false;
    if (!code->is_string() || !message->is_string() || !origin->is_string()) return false;
    if (!isRejectionCode(code->get<std::string>())) return false;
    out.code    = code->get<std::string>();
    out.message = message->get<std::string>();
    out.origin  = origin->get<std::string>();
    return true;
}

LogosMap callEnvelope(const std::string& module,
                      const std::string& method,
                      const nlohmann::json& ret,
                      const nlohmann::json& args,
                      CallFailure failure,
                      const MethodLister& listMethods)
{
    LogosMap result;

    // A provider that ran and REFUSED answers through the result rather than
    // the error channel, so fold that in before deciding: both are failures of
    // the call and must read identically to whoever asked.
    if (failure.ok()) dispatchRejection(ret, failure);

    // THE NULL RETURN, and the one round-trip that is allowed to explain it.
    //
    // Every provider flavour answers an unknown method name with a bare null,
    // byte-identical to a method that legitimately returns null.
    // logos_protocol.h says so in as many words ("NOT reported, and it is not
    // an oversight: an unknown method name"), and the cdylib dispatch ends in
    // `return nullptr;  // unknown method` (lidl_gen_cdylib.cpp). No transport
    // can separate the two -- but core_service can ASK, because the module
    // publishes its own interface. That happens only on a null return, so the
    // ordinary path is unaffected.
    //
    // The SAME answer settles the second way a call goes quiet, which is the
    // one an operator actually meets: an argument whose type the declared
    // parameter cannot take. `logoscore call keystore_module has_address` with
    // a number for its `tstr` came back {"result": null, "status": "ok"} --
    // exit code 0, nothing logged, and no layer between the two ends saying a
    // word. The method list already in hand names the parameter types, so the
    // sentence is there to be said; see argumentMismatch for how narrowly.
    //
    // Stay silent when introspection fails or comes back empty: an unproven
    // METHOD_NOT_FOUND would just be the old null-means-failure guess wearing a
    // better name, and an unproven argument complaint would refuse a working
    // call outright.
    if (failure.ok() && ret.is_null() && listMethods) {
        const std::vector<MethodInfo> methods = listMethods();
        const MethodInfo* found = nullptr;
        for (const MethodInfo& m : methods)
            if (m.name == method) { found = &m; break; }

        if (!methods.empty() && !found) {
            std::vector<std::string> names;
            names.reserve(methods.size());
            for (const MethodInfo& m : methods) names.push_back(m.name);
            const std::string msg =
                "Method '" + method + "' not found on module '" + module + "'.";
            result["status"]            = "error";
            result["code"]              = "METHOD_NOT_FOUND";
            result["message"]           = msg;
            result["available_methods"] = names;   // docs/spec.md's envelope
            return result;
        }

        std::string reason;
        if (found && argumentMismatch(*found, args, reason))
            failure = CallFailure{"argument_mismatch", reason, module};
    }

    if (!failure.ok()) {
        // ONE code for every transport-detected failure, exactly as before:
        // object_unavailable / timeout / transport_error / call_failed /
        // unauthorized, plus the folded provider refusal and the argument
        // mismatch read off the module's own interface. The specific code
        // rides in `error` so a JSON consumer can tell them apart without
        // parsing prose, and is appended to the message for a human reader.
        const std::string msg = "Call to " + module + "." + method + " failed ("
                              + failure.code + ": " + failure.message + ").";
        result["status"]  = "error";
        result["code"]    = "METHOD_FAILED";
        result["message"] = msg;
        result["error"]   = LogosMap{{"code",    failure.code},
                                     {"message", failure.message},
                                     {"origin",  failure.origin}};
        return result;
    }

    // Success — INCLUDING a null result. `null` is a value here: an empty
    // optional, or a method that returns nothing in particular. It stopped
    // meaning "the call failed" when this function started reading the error
    // channel instead of the value.
    result["status"] = "ok";
    result["module"] = module;
    result["method"] = method;
    result["result"] = ret;
    return result;
}

} // namespace core_service
