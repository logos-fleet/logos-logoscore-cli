// The proxied-call envelope: ok vs METHOD_FAILED vs METHOD_NOT_FOUND.
//
// The whole point of this file is ONE distinction. `logosctl call` used to
// report METHOD_FAILED whenever the module answered null, because
// core_service_impl.cpp reached for the invokeRemoteMethod overload that has no
// logos::CallError* parameter and therefore had nothing but the value to judge
// by. A method that legitimately returns nothing was indistinguishable from a
// call that never ran.
//
// The error channel already carried that distinction everywhere else — lp_invoke
// branches on callErr.ok() and never on the value — so these tests pin the two
// answers apart at the layer that used to conflate them:
//
//   transport reported a failure        -> METHOD_FAILED   (+ error.code)
//   provider RAN and refused            -> METHOD_FAILED   (+ its rejection code)
//   module has no such method           -> METHOD_NOT_FOUND (+ available_methods)
//   method exists and answered null     -> ok, result null   <- the change
//
// No Qt, no transport, no daemon: callEnvelope is pure, and the introspection
// hop it needs for the third case arrives as a callback these tests supply.

#include <gtest/gtest.h>

#include "core_service/call_envelope.h"

using core_service::CallFailure;
using core_service::callEnvelope;
using core_service::dispatchRejection;

namespace {

// An introspection hook that records whether it was consulted, so the tests can
// assert the ordinary path does NOT pay for the extra round-trip.
//
// A bare NAME publishes no parameter list, which is the shape a module that
// describes only its method names produces — and the shape every case that
// predates the argument check wants, because it makes argumentMismatch silent.
struct Lister {
    std::vector<core_service::MethodInfo> methods;
    mutable int calls = 0;

    Lister(std::initializer_list<const char*> names)
    {
        for (const char* n : names) {
            core_service::MethodInfo m;
            m.name = n;
            methods.push_back(m);
        }
    }
    explicit Lister(std::vector<core_service::MethodInfo> ms)
        : methods(std::move(ms)) {}

    core_service::MethodLister fn() const {
        return [this]() { ++calls; return methods; };
    }
};

const Lister kBasicModule{"returnTrue", "returnNothing", "echo"};

// One method with a published parameter list.
core_service::MethodInfo typed(const char* name,
                               std::vector<std::string> paramTypes)
{
    core_service::MethodInfo m;
    m.name = name;
    m.paramTypes = std::move(paramTypes);
    m.paramsPublished = true;
    return m;
}

// The call shape the cases below the argument check use: no arguments sent, so
// nothing for it to judge.
LogosMap callEnv(const std::string& module, const std::string& method,
                 const nlohmann::json& ret, CallFailure failure,
                 const core_service::MethodLister& lister)
{
    return core_service::callEnvelope(module, method, ret,
                                      nlohmann::json::array(), failure, lister);
}

} // namespace

// ── The behaviour change: null is a VALUE, not a failure ────────────────────

TEST(CallEnvelope, NullFromAKnownMethodIsOk)
{
    const LogosMap env = callEnv("test_basic_module", "returnNothing",
                                      nlohmann::json(), CallFailure{},
                                      kBasicModule.fn());

    EXPECT_EQ(env.value("status", std::string{}), "ok");
    EXPECT_EQ(env.value("module", std::string{}), "test_basic_module");
    EXPECT_EQ(env.value("method", std::string{}), "returnNothing");
    ASSERT_TRUE(env.contains("result"));
    EXPECT_TRUE(env["result"].is_null())
        << "a null return must survive as a null RESULT, not become an error";
    EXPECT_FALSE(env.contains("code"));
}

TEST(CallEnvelope, FalseyValuesAreOkToo)
{
    // These all used to be safe (only `null` tripped the old check), but they
    // are the neighbours of the case that broke, so pin them.
    for (const nlohmann::json v : {nlohmann::json(false), nlohmann::json(0),
                                   nlohmann::json(""), nlohmann::json::array(),
                                   nlohmann::json::object()}) {
        const LogosMap env = callEnv("m", "echo", v, CallFailure{},
                                          kBasicModule.fn());
        EXPECT_EQ(env.value("status", std::string{}), "ok") << v.dump();
        EXPECT_EQ(env.value("result", nlohmann::json()), v) << v.dump();
    }
}

TEST(CallEnvelope, OrdinaryCallNeverIntrospects)
{
    Lister lister{"echo"};
    callEnv("m", "echo", nlohmann::json("hi"), CallFailure{}, lister.fn());
    EXPECT_EQ(lister.calls, 0)
        << "a non-null result must cost exactly one round-trip";
}

// ── A genuinely failed call still reports METHOD_FAILED ─────────────────────

TEST(CallEnvelope, TransportFailureIsMethodFailed)
{
    // Every code logos::CallError can carry. All of them are failures of the
    // call, and all of them keep the single documented METHOD_FAILED code.
    for (const char* code : {"object_unavailable", "timeout", "transport_error",
                             "call_failed", "unauthorized"}) {
        Lister lister{"returnTrue"};
        const LogosMap env = callEnv(
            "test_basic_module", "returnTrue", nlohmann::json(),
            CallFailure{code, "the transport said so", "test_basic_module"},
            lister.fn());

        EXPECT_EQ(env.value("status", std::string{}), "error") << code;
        EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED") << code;
        // The specific failure stays machine-readable rather than being
        // flattened into prose.
        ASSERT_TRUE(env.contains("error")) << code;
        EXPECT_EQ(env["error"].value("code", std::string{}), code);
        EXPECT_EQ(env["error"].value("origin", std::string{}), "test_basic_module");
        EXPECT_NE(env.value("message", std::string{}).find(code), std::string::npos)
            << "the human message should name the underlying failure";
        EXPECT_EQ(lister.calls, 0)
            << "a failed call must not go back to a module that just failed";
    }
}

TEST(CallEnvelope, FailureWinsOverAValue)
{
    // A transport failure can still hand back a value; the error channel is
    // what decides, in both directions.
    const LogosMap env = callEnv("m", "echo", nlohmann::json("leftover"),
                                      CallFailure{"timeout", "took too long", "m"},
                                      kBasicModule.fn());
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
}

// ── A provider that RAN and refused ─────────────────────────────────────────

TEST(CallEnvelope, DispatchRejectionIsMethodFailed)
{
    const nlohmann::json refusal{{"code", "dispatch_failed"},
                                 {"message", "wrong argument count"},
                                 {"origin", "test_basic_module"}};

    const LogosMap env = callEnv("test_basic_module", "isPositive", refusal,
                                      CallFailure{}, kBasicModule.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
    ASSERT_TRUE(env.contains("error"));
    EXPECT_EQ(env["error"].value("code", std::string{}), "dispatch_failed");
    EXPECT_EQ(env["error"].value("message", std::string{}), "wrong argument count");
}

// The LIVE bug this widening fixes. A provider answers an arity error with
// {"code":"invalid_args", ...} as its RESULT — logos-cpp-sdk's cdylib dispatch
// and logos-rust-sdk's args::invalid_args both do, and have all along. Before
// the detector matched a closed SET rather than the single literal
// "dispatch_failed", this envelope came back status "ok" with the refusal as
// the value, and `logosctl call` exited 0. Measured, on
// `logosctl call test_basic_module isPositive` with the argument missing.
TEST(CallEnvelope, InvalidArgsIsMethodFailed)
{
    const nlohmann::json refusal{{"code", "invalid_args"},
                                 {"message", "expected 1 arguments, got 0"},
                                 {"origin", "test_basic_module"}};

    const LogosMap env = callEnv("test_basic_module", "isPositive", refusal,
                                      CallFailure{}, kBasicModule.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
    ASSERT_TRUE(env.contains("error"));
    EXPECT_EQ(env["error"].value("code", std::string{}), "invalid_args");
    EXPECT_EQ(env["error"].value("message", std::string{}),
              "expected 1 arguments, got 0");
    // The refusal must NOT also survive as a value: an envelope carrying both
    // would let a caller keep reading it as data.
    EXPECT_FALSE(env.contains("result"));
}

// Every code in the closed set folds, not just dispatch_failed. "unknown_method"
// is here before any provider emits it — that readiness is the point of doing
// the detectors first.
TEST(CallEnvelope, EveryRejectionCodeIsMethodFailed)
{
    for (const char* code : {"dispatch_failed", "invalid_args", "unknown_method"}) {
        CallFailure out;
        EXPECT_TRUE(dispatchRejection(nlohmann::json{{"code", code},
                                                     {"message", "m"},
                                                     {"origin", "o"}}, out))
            << code;
        EXPECT_EQ(out.code, code);
        EXPECT_EQ(out.message, "m");
        EXPECT_EQ(out.origin, "o");

        const LogosMap env = callEnv("test_basic_module", "isPositive",
                                          nlohmann::json{{"code", code},
                                                         {"message", "m"},
                                                         {"origin", "o"}},
                                          CallFailure{}, kBasicModule.fn());
        EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED") << code;
    }
}

// The NEGATIVES. Widening one literal into a set is one careless edit away from
// "any object with a code", which would hand every method that legitimately
// returns a three-string map to the error channel. These are what keep the
// match closed.
TEST(CallEnvelope, DispatchRejectionMatchStaysNarrow)
{
    CallFailure out;
    // Right shape, right code.
    EXPECT_TRUE(dispatchRejection(nlohmann::json{{"code", "dispatch_failed"},
                                                 {"message", "m"},
                                                 {"origin", "o"}}, out));

    // A code OUTSIDE the closed set stays DATA, however plausible. This is the
    // difference between a closed set and an open shape match.
    for (const char* code : {"", "ok", "not_found", "user_error",
                             "DISPATCH_FAILED", "dispatch_failed ",
                             "invalid_argument", "unknown_methods"}) {
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"},
                                                      {"origin", "o"}}, out))
            << code;
    }

    // Wrong key COUNT, for every code in the set — 2 keys and 4 keys.
    for (const char* code : {"dispatch_failed", "invalid_args", "unknown_method"}) {
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"}}, out))
            << code;
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"},
                                                      {"origin", "o"},
                                                      {"extra", 1}}, out))
            << code;
        // A non-string in any of the three slots.
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", 7},
                                                      {"message", "m"},
                                                      {"origin", "o"}}, out))
            << code;
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", 7},
                                                      {"origin", "o"}}, out))
            << code;
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"},
                                                      {"origin", nullptr}}, out))
            << code;
    }

    EXPECT_FALSE(dispatchRejection(nlohmann::json::array(), out));
    EXPECT_FALSE(dispatchRejection(nlohmann::json(), out));

    // And end-to-end: an unrecognised three-string map is a RESULT, not an error.
    const nlohmann::json userMap{{"code", "amber"},
                                 {"message", "hello"},
                                 {"origin", "sensor"}};
    const LogosMap env = callEnv("test_basic_module", "describe", userMap,
                                      CallFailure{}, kBasicModule.fn());
    EXPECT_EQ(env.value("status", std::string{}), "ok");
    EXPECT_EQ(env["result"], userMap);
}

// ── An unknown method: the one case the wire cannot report ──────────────────

TEST(CallEnvelope, UnknownMethodIsMethodNotFound)
{
    // Providers answer an unknown method with a bare null and no error — see
    // logos_protocol.h and lidl_gen_cdylib.cpp's `return nullptr; // unknown
    // method`. Only the module's own method list can settle it.
    Lister lister{"returnTrue", "echo"};
    const LogosMap env = callEnv("test_basic_module", "noSuchMethod",
                                      nlohmann::json(), CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_NOT_FOUND");
    EXPECT_EQ(env.value("message", std::string{}),
              "Method 'noSuchMethod' not found on module 'test_basic_module'.");
    ASSERT_TRUE(env.contains("available_methods"));
    EXPECT_EQ(env["available_methods"],
              nlohmann::json::array({"returnTrue", "echo"}));
    EXPECT_EQ(lister.calls, 1) << "introspection must be consulted exactly once";
}

TEST(CallEnvelope, UnprovableMissingMethodStaysOk)
{
    // Introspection unavailable (module wedged, transport dropped, provider
    // that publishes nothing). We cannot prove the method is missing, so we do
    // not claim it — that would be the old null-means-failure guess wearing a
    // better name.
    Lister lister{};
    const LogosMap env = callEnv("m", "whoKnows", nlohmann::json(),
                                      CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("status", std::string{}), "ok");
    ASSERT_TRUE(env.contains("result"));
    EXPECT_TRUE(env["result"].is_null());
    EXPECT_EQ(lister.calls, 1);
}

TEST(CallEnvelope, NoListerAtAllStaysOk)
{
    const LogosMap env = callEnv("m", "whoKnows", nlohmann::json(),
                                      CallFailure{}, nullptr);
    EXPECT_EQ(env.value("status", std::string{}), "ok");
}

// ── The distinction, stated as one assertion ────────────────────────────────

TEST(CallEnvelope, EmptyAndFailedAreDistinguishable)
{
    const LogosMap empty = callEnv("m", "returnNothing", nlohmann::json(),
                                        CallFailure{}, kBasicModule.fn());
    const LogosMap failed = callEnv(
        "m", "returnNothing", nlohmann::json(),
        CallFailure{"object_unavailable", "module is gone", "m"},
        kBasicModule.fn());

    // Same method, same null on the wire, opposite answers — which is the
    // entire point of reading the error channel.
    EXPECT_NE(empty.value("status", std::string{}),
              failed.value("status", std::string{}));
    EXPECT_EQ(empty.value("status", std::string{}), "ok");
    EXPECT_EQ(failed.value("code", std::string{}), "METHOD_FAILED");
}

// ── An argument the signature cannot take: the OTHER silent null ────────────
//
// `logoscore call keystore_module has_address <a number>` answered
// {"result": null, "status": "ok"} — exit code 0, nothing logged by the daemon,
// and a script reading the exit code saw success. The method EXISTS, so
// METHOD_NOT_FOUND does not apply; the module list already in hand names the
// parameter types, so the envelope can say what went wrong instead of nothing.

TEST(CallEnvelope, NumberForAStringParameterIsMethodFailed)
{
    const Lister lister{{typed("has_address", {"QString"})}};
    const LogosMap env = core_service::callEnvelope(
        "keystore_module", "has_address", nlohmann::json(),
        nlohmann::json::array({7.9250088148318908e+47}),
        CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
    ASSERT_TRUE(env.contains("error"));
    EXPECT_EQ(env["error"].value("code", std::string{}), "argument_mismatch");
    // logos-protocol's own wording, so a provider's sentence and this one read
    // the same to whoever has to act on it.
    EXPECT_EQ(env["error"].value("message", std::string{}),
              "expected string at arg0, got number");
    EXPECT_EQ(env["error"].value("origin", std::string{}), "keystore_module");
    EXPECT_EQ(lister.calls, 1);
}

TEST(CallEnvelope, TheOFFENDINGArgumentIsNamedByPosition)
{
    const Lister lister{{typed("timed_unlock", {"QString", "QString", "int"})}};
    const LogosMap env = core_service::callEnvelope(
        "keystore_module", "timed_unlock", nlohmann::json(),
        nlohmann::json::array({"0xabc", "hunter2", "not-a-number"}),
        CallFailure{}, lister.fn());

    EXPECT_EQ(env["error"].value("message", std::string{}),
              "expected integer at arg2, got string");
}

TEST(CallEnvelope, EveryDeclaredTypeRefusesTheWrongShape)
{
    struct Case { const char* qtType; nlohmann::json arg; const char* expected; };
    const std::vector<Case> cases{
        {"QString",      nlohmann::json(1),                 "expected string at arg0, got number"},
        {"QString",      nlohmann::json(true),              "expected string at arg0, got boolean"},
        {"bool",         nlohmann::json(1),                 "expected bool at arg0, got number"},
        {"bool",         nlohmann::json("true"),            "expected bool at arg0, got string"},
        {"int",          nlohmann::json("42"),              "expected integer at arg0, got string"},
        {"double",       nlohmann::json("1.5"),             "expected number at arg0, got string"},
        {"QVariantList", nlohmann::json("notalist"),        "expected array at arg0, got string"},
        {"QStringList",  nlohmann::json::object(),          "expected array at arg0, got object"},
        {"QVariantMap",  nlohmann::json::array({1}),        "expected object at arg0, got array"},
    };
    for (const Case& c : cases) {
        const Lister lister{{typed("f", {c.qtType})}};
        const LogosMap env = core_service::callEnvelope(
            "m", "f", nlohmann::json(), nlohmann::json::array({c.arg}),
            CallFailure{}, lister.fn());
        EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED") << c.qtType;
        EXPECT_EQ(env["error"].value("message", std::string{}), c.expected) << c.qtType;
    }
}

// ── And the silences, which is where a check like this earns its keep ───────

TEST(CallEnvelope, AMatchingArgumentIsStillOk)
{
    // The address that started all this, as the string it now is: the method
    // legitimately answered null, and that must survive as a null result.
    const Lister lister{{typed("has_address", {"QString"})}};
    const LogosMap env = core_service::callEnvelope(
        "keystore_module", "has_address", nlohmann::json(),
        nlohmann::json::array({"0x8ad0Fcf71D6FBD060BAfd45f5155b1e52d3591C5"}),
        CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("status", std::string{}), "ok");
    EXPECT_TRUE(env["result"].is_null());
}

TEST(CallEnvelope, AnUnknownDeclaredTypeIsNeverSecondGuessed)
{
    // "QVariant" is what `any` and every `?T` publish as, and it accepts
    // anything. Refusing here would refuse calls that work.
    for (const char* t : {"QVariant", "QByteArray", "LogosResult", "SomeEnum"}) {
        const Lister lister{{typed("f", {t})}};
        const LogosMap env = core_service::callEnvelope(
            "m", "f", nlohmann::json(), nlohmann::json::array({42}),
            CallFailure{}, lister.fn());
        EXPECT_EQ(env.value("status", std::string{}), "ok") << t;
    }
}

TEST(CallEnvelope, ANullArgumentIsOptionalsEmptyState)
{
    const Lister lister{{typed("f", {"QString"})}};
    const LogosMap env = core_service::callEnvelope(
        "m", "f", nlohmann::json(), nlohmann::json::array({nullptr}),
        CallFailure{}, lister.fn());
    EXPECT_EQ(env.value("status", std::string{}), "ok");
}

TEST(CallEnvelope, AWrongARITYIsLeftToTheProvider)
{
    // A COUNT is class B and the provider answers it with invalid_args. Two
    // layers reporting the same thing differently is worse than one.
    const Lister lister{{typed("f", {"QString", "QString"})}};
    for (const nlohmann::json args : {nlohmann::json::array({1}),
                                      nlohmann::json::array({1, 2, 3})}) {
        const LogosMap env = core_service::callEnvelope(
            "m", "f", nlohmann::json(), args, CallFailure{}, lister.fn());
        EXPECT_EQ(env.value("status", std::string{}), "ok") << args.dump();
    }
}

TEST(CallEnvelope, AModuleThatPublishesNoParameterListIsNeverJudged)
{
    // Bare names only — paramsPublished is false, so there is nothing to
    // compare against and the envelope says nothing.
    const Lister lister{"f"};
    const LogosMap env = core_service::callEnvelope(
        "m", "f", nlohmann::json(), nlohmann::json::array({42}),
        CallFailure{}, lister.fn());
    EXPECT_EQ(env.value("status", std::string{}), "ok");
}

TEST(CallEnvelope, AProviderRejectionStillWins)
{
    // The provider spoke first; its own code and sentence are the ones that
    // reach the caller, not this function's guess at the same fault.
    const Lister lister{{typed("f", {"QString"})}};
    const nlohmann::json refusal{{"code", "dispatch_failed"},
                                 {"message", "expected string at arg0, got number"},
                                 {"origin", "m"}};
    const LogosMap env = core_service::callEnvelope(
        "m", "f", refusal, nlohmann::json::array({42}), CallFailure{}, lister.fn());

    EXPECT_EQ(env["error"].value("code", std::string{}), "dispatch_failed");
    EXPECT_EQ(lister.calls, 0) << "a refusal is already an answer; do not go back to ask";
}

TEST(CallEnvelope, AnUnknownMethodIsStillReportedAsMissingNotMistyped)
{
    // Both checks live on the same null return; the method's absence is the
    // more useful of the two answers and must be the one given.
    const Lister lister{{typed("has_address", {"QString"})}};
    const LogosMap env = core_service::callEnvelope(
        "keystore_module", "hasAddress", nlohmann::json(),
        nlohmann::json::array({42}), CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("code", std::string{}), "METHOD_NOT_FOUND");
    EXPECT_EQ(env["available_methods"], nlohmann::json::array({"has_address"}));
}
