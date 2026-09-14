#ifndef CORE_SERVICE_CALL_ENVELOPE_H
#define CORE_SERVICE_CALL_ENVELOPE_H

// How a proxied module call becomes the `logosctl call` JSON envelope.
//
// Split out of core_service_impl.cpp because it is the part that was WRONG and
// the part worth pinning: given what the transport reported and what the module
// answered, decide ok / METHOD_FAILED / METHOD_NOT_FOUND. Nothing here touches
// Qt, logos-protocol or a live transport, so the whole decision is exercised by
// the unit suite (tests/test_call_envelope.cpp) instead of only by a
// daemon-backed integration run.

#include <logos_json.h>

#include <functional>
#include <string>
#include <vector>

namespace core_service {

// {code, message, origin}; an empty code means "no error".
//
// The std mirror of logos::CallError (logos-protocol/cpp/logos_call_error.h) —
// deliberately a separate type so this header stays free of the protocol
// include, which the unit-test library does not link. core_service_impl.cpp
// copies the three fields across at the one call site.
struct CallFailure {
    std::string code;
    std::string message;
    std::string origin;

    bool ok() const { return code.empty(); }
};

// True when `v` is the canonical provider REJECTION object rather than a value;
// fills `out` with its {code, message, origin} on a match.
//
// A provider that RAN and refused the call answers
// {"code":<rejection code>, "message":..., "origin":...} as its RESULT, not
// through the transport's error channel — logos_protocol.h states that split
// explicitly. Generated typed consumers fold it into logos::CallError
// themselves (emitDispatchRejectionDetectorJson in logos-cpp-sdk's
// generator_lib.cpp, and lidl_gen_qt_consumer.cpp's byte-identical twin);
// `logosctl call` is the UNTYPED consumer of that same surface, so it has to
// fold it here or a refusal would read as a successful call that returned a
// three-key map.
//
// `code` is matched against a CLOSED SET, defined in the .cpp:
//
//   "dispatch_failed" — the provider ran and refused the argument VALUES.
//   "invalid_args"    — wrong argument COUNT. Providers have emitted this all
//                       along (logos-cpp-sdk's cdylib dispatch, logos-rust-sdk's
//                       args::invalid_args) and no detector matched it, so
//                       `logosctl call test_basic_module isPositive` with the
//                       argument missing exited 0 with status "ok" and the
//                       refusal object as its result.
//   "unknown_method"  — NOTHING EMITS THIS YET. Listed now because widening a
//                       detector is backwards-compatible on its own, whereas a
//                       new provider code shipped against narrow detectors
//                       arrives at consumers as data.
//
// WHEN A PROVIDER STARTS EMITTING "unknown_method", READ THIS. It will fold to
// METHOD_FAILED here, not to the METHOD_NOT_FOUND envelope callEnvelope already
// builds from introspection below — the two paths are independent today and
// nothing makes them agree. Deliberately left alone: no provider emits the code,
// so any routing written now would be untested against a real provider, and
// choosing between the two envelopes is part of the provider-contract change,
// not of widening a detector.
//
// The match is otherwise unchanged and stays NARROW — exactly three fields, all
// strings — for the same reason the generated detector is: a map return
// carrying user data must never false-match. An unrecognised code, a 2- or
// 4-key object, and a non-string value all stay DATA.
bool dispatchRejection(const nlohmann::json& v, CallFailure& out);

// One method as the module describes itself through getPluginMethods.
struct MethodInfo {
    std::string name;
    // The DECLARED parameter types, verbatim in whatever VOCABULARY the module
    // published them in. There are two, and they are not distinguishable from
    // the string:
    //   - Qt type names ("QString", "int", "double", "bool", "QVariantList",
    //     "QVariantMap", "QByteArray", "QVariant") -- a handcrafted Qt plugin
    //     answering from its QMetaObject, and logos-rust-sdk's generated
    //     provider (lidl-gen's qt_type_name). `?T` and `any` land on
    //     "QVariant", which is exactly the answer "no rule here".
    //   - LIDL contract names ("tstr", "uint", "[Point]", "? tstr") -- the C++
    //     cdylib backend, which publishes its contract rather than Qt's
    //     spelling of it (lidl_gen_cdylib's lidlTypeToPublishedName).
    // argumentMismatch never has to tell them apart: a name it does not know
    // is silence, so the LIDL half is simply not judged. See the table in
    // call_envelope.cpp for why widening it is not a free change.
    std::vector<std::string> paramTypes;
    // Whether the module published a parameter list at all. A method with no
    // parameters has none, and that is NOT the same as a module that does not
    // describe its parameters -- the first licenses an arity comparison, the
    // second does not.
    bool paramsPublished = false;
};

// Supplies the module's exposed methods, or an empty vector when the module
// could not be asked. callEnvelope invokes this AT MOST ONCE, and only on the
// single path that needs it, so the ordinary call still costs exactly one
// round-trip.
using MethodLister = std::function<std::vector<MethodInfo>()>;

// Whether `args` can satisfy `method`'s declared parameter list, and if not,
// why -- in logos-protocol's own wording ("expected string at arg0, got
// number"), so a reader cannot tell this sentence apart from a provider's.
//
// CONSERVATIVE BY CONSTRUCTION, because a false positive here would refuse a
// call that works. It answers "mismatch" only when the module published a
// parameter list, the arity agrees (a count is class B and the provider's job),
// the declared type has an unambiguous JSON counterpart, and the value is not
// null (null is `?T`'s empty state, and no declared type rules it out from
// here). Everything else is silence.
bool argumentMismatch(const MethodInfo& method,
                      const nlohmann::json& args,
                      std::string& reason);

// The envelope for one proxied call.
//
//   {"status":"ok","module":...,"method":...,"result":<value>}
//   {"status":"error","code":"METHOD_FAILED","message":...,"error":{...}}
//        error.code is the transport's, or the provider's own rejection code,
//        or "argument_mismatch" -- this function's own, and the only one in
//        the set that no provider ever said.
//   {"status":"error","code":"METHOD_NOT_FOUND","message":...,
//    "available_methods":[...]}
//
// `failure` is what the transport reported (see CallFailure); `ret` is the
// value the module answered with, already decoded to JSON; `args` is what was
// sent, needed only to explain a null return (see argumentMismatch).
LogosMap callEnvelope(const std::string& module,
                      const std::string& method,
                      const nlohmann::json& ret,
                      const nlohmann::json& args,
                      CallFailure failure,
                      const MethodLister& listMethods);

} // namespace core_service

#endif // CORE_SERVICE_CALL_ENVELOPE_H
