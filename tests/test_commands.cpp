#include <gtest/gtest.h>

#include <filesystem>
#include <logos_json.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include "client/client.h"
#include "client/client_state.h"
#include "client/output.h"
#include "client/commands/command.h"
#include "client/commands/package_command.h"
#include "config.h"
#include "daemon/daemon_state.h"

// Mock client for testing commands without a real daemon
class MockClient : public Client {
public:
    // Control mock behavior
    bool shouldConnect = true;
    std::string connectError = "No running logosctl daemon. Start one with: logosctl -D";
    LogosMap  loadModuleResult;
    LogosMap  unloadModuleResult;
    LogosMap  reloadModuleResult;
    // Default to a PRESENT, empty list so "the test never set this" keeps
    // meaning "the daemon answered with nothing". A test that wants a failed
    // RPC assigns std::nullopt explicitly.
    std::optional<LogosList> listModulesResult = LogosList::array();
    LogosMap  statusResult;
    LogosMap  moduleInfoResult;
    std::optional<LogosList> moduleStatsResult = LogosList::array();
    LogosMap  callMethodResult;
    // `package show` makes two different calls -- installed list, then
    // catalog -- so one canned reply cannot express both. Keyed entries win
    // over `callMethodResult`.
    std::map<std::string, LogosMap> callMethodResultByMethod;
    LogosMap  shutdownResult;
    LogosMap  refreshModulesResult;
    LogosMap  planPackageResult;
    LogosMap  applyPackageResult;
    LogosMap  downloadResult;

    // Track calls
    //
    // `connectAttempts` / `rpcCalls` are the two the stale-session cases read.
    // A guard that fires has to be visible as the ABSENCE of contact -- the
    // whole point is that no byte goes to a socket nobody is listening on --
    // and per-method breadcrumbs like `lastLoadedModule` cannot express that
    // for a command whose method takes no distinguishing argument.
    int  connectAttempts = 0;
    int  rpcCalls = 0;
    bool shutdownCalled = false;
    bool refreshModulesCalled = false;
    bool applyPackageCalled = false;
    std::string lastPackageOp;
    LogosList   lastPackageNames;
    LogosMap    lastPackageOpts;
    std::string lastDownloadName;
    LogosMap    lastDownloadOpts;
    std::string lastLoadedModule;
    std::string lastUnloadedModule;
    // Defaults to true so a test that never sets --no-dependents still sees
    // the production default rather than a value-initialised false.
    bool lastUnloadWithDependents = true;
    std::string lastReloadedModule;
    std::string lastInfoModule;
    std::string lastCallModule;
    std::string lastCallMethod;
    LogosList   lastCallArgs;
    std::string lastListFilter;
    std::string lastWatchModule;
    std::string lastWatchEventName;
    bool watchShouldSucceed = false;

    bool connect() override {
        ++connectAttempts;
        m_connected = shouldConnect;
        m_lastError = shouldConnect ? "" : connectError;
        return shouldConnect;
    }

    bool isConnected() const override { return m_connected; }
    std::string lastError() const override { return m_lastError; }

    LogosMap loadModule(const std::string& name) override {
        ++rpcCalls;
        lastLoadedModule = name;
        return loadModuleResult;
    }

    LogosMap unloadModule(const std::string& name, bool withDependents) override {
        ++rpcCalls;
        lastUnloadedModule = name;
        lastUnloadWithDependents = withDependents;
        return unloadModuleResult;
    }

    LogosMap refreshModules() override {
        ++rpcCalls;
        refreshModulesCalled = true;
        return refreshModulesResult;
    }

    LogosMap planPackageOperation(const std::string& op, const LogosList& names,
                                  const LogosMap& opts) override {
        ++rpcCalls;
        lastPackageOp = op;
        lastPackageNames = names;
        lastPackageOpts = opts;
        return planPackageResult;
    }

    LogosMap applyPackageOperation(const std::string& op, const LogosList& names,
                                   const LogosMap& opts) override {
        ++rpcCalls;
        lastPackageOp = op;
        lastPackageNames = names;
        lastPackageOpts = opts;
        applyPackageCalled = true;
        return applyPackageResult;
    }

    LogosMap downloadPackage(const std::string& name, const LogosMap& opts) override {
        ++rpcCalls;
        lastDownloadName = name;
        lastDownloadOpts = opts;
        return downloadResult;
    }

    LogosMap reloadModule(const std::string& name) override {
        ++rpcCalls;
        lastReloadedModule = name;
        return reloadModuleResult;
    }

    std::optional<LogosList> listModules(const std::string& filter) override {
        ++rpcCalls;
        lastListFilter = filter;
        return listModulesResult;
    }

    LogosMap getStatus() override { ++rpcCalls; return statusResult; }

    LogosMap getModuleInfo(const std::string& name) override {
        ++rpcCalls;
        lastInfoModule = name;
        return moduleInfoResult;
    }

    std::optional<LogosList> getModuleStats() override { ++rpcCalls; return moduleStatsResult; }

    LogosMap callModuleMethod(const std::string& module, const std::string& method,
                               const LogosList& args) override {
        ++rpcCalls;
        lastCallModule = module;
        lastCallMethod = method;
        lastCallArgs   = args;
        auto it = callMethodResultByMethod.find(method);
        if (it != callMethodResultByMethod.end()) return it->second;
        return callMethodResult;
    }

    LogosMap shutdown() override {
        ++rpcCalls;
        shutdownCalled = true;
        return shutdownResult;
    }

    bool watchModuleEvents(const std::string& module, const std::string& eventName,
                            std::function<void(const LogosMap&)> callback) override {
        ++rpcCalls;
        (void)callback;
        lastWatchModule    = module;
        lastWatchEventName = eventName;
        return m_connected && watchShouldSucceed;
    }

private:
    bool m_connected = false;
    std::string m_lastError;
};

class CommandTest : public ::testing::Test {
protected:
    MockClient mockClient;
    Output output{true}; // Force JSON mode for testable output

    // Commands consult the session on disk (StopCommand reads
    // daemon/state.json to tell a live daemon from a stale one), so every
    // case runs against an empty session of its own. Without this the suite
    // reads whatever ~/.logosctl the developer happens to have, and a session
    // left over from last week decides whether `stop` succeeds. Mirrors
    // DaemonStateTest, which isolates the same three layers.
    std::string origHome;
    std::string origConfigDir;
    bool        origConfigDirSet = false;
    std::filesystem::path testDir;

    void SetUp() override {
        mockClient.shouldConnect = true;

        testDir = std::filesystem::temp_directory_path()
                / ("logosctl_test_cmd_" + std::to_string(getpid()));
        std::filesystem::create_directories(testDir);

        const char* home = std::getenv("HOME");
        origHome = home ? home : "";
        setenv("HOME", testDir.c_str(), 1);

        const char* cd = std::getenv("LOGOSCTL_CONFIG_DIR");
        origConfigDirSet = cd != nullptr;
        origConfigDir = origConfigDirSet ? cd : "";
        unsetenv("LOGOSCTL_CONFIG_DIR");

        Config::setConfigDir(testDir.string());
    }

    void TearDown() override {
        ClientStateFile::setOverride(std::nullopt);
        Config::setConfigDir("");
        setenv("HOME", origHome.c_str(), 1);
        if (origConfigDirSet)
            setenv("LOGOSCTL_CONFIG_DIR", origConfigDir.c_str(), 1);
        else
            unsetenv("LOGOSCTL_CONFIG_DIR");
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    std::string captureOutput(std::function<void()> fn) {
        std::stringstream buffer;
        auto oldBuf = std::cout.rdbuf(buffer.rdbuf());
        fn();
        std::cout.rdbuf(oldBuf);
        return buffer.str();
    }

    nlohmann::json parseJson(const std::string& s) {
        return nlohmann::json::parse(s);
    }
};

// ── createCommand ────────────────────────────────────────────────────────────

TEST_F(CommandTest, CreateCommand_KnownCommands)
{
    EXPECT_NE(createCommand("status",        mockClient, output), nullptr);
    EXPECT_NE(createCommand("load-module",   mockClient, output), nullptr);
    EXPECT_NE(createCommand("unload-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("reload-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("list-modules",  mockClient, output), nullptr);
    EXPECT_NE(createCommand("module-info",   mockClient, output), nullptr);
    EXPECT_NE(createCommand("info",          mockClient, output), nullptr);
    EXPECT_NE(createCommand("call",          mockClient, output), nullptr);
    EXPECT_NE(createCommand("module",        mockClient, output), nullptr);
    EXPECT_NE(createCommand("watch",         mockClient, output), nullptr);
    EXPECT_NE(createCommand("stats",         mockClient, output), nullptr);
    EXPECT_NE(createCommand("stop",          mockClient, output), nullptr);
    EXPECT_NE(createCommand("issue-token",   mockClient, output), nullptr);
    EXPECT_NE(createCommand("revoke-token",  mockClient, output), nullptr);
    EXPECT_NE(createCommand("list-tokens",   mockClient, output), nullptr);
}

TEST_F(CommandTest, CreateCommand_Unknown_ReturnsNull)
{
    EXPECT_EQ(createCommand("nonexistent", mockClient, output), nullptr);
}

// ── knownSubcommands ─────────────────────────────────────────────────────────

TEST_F(CommandTest, KnownSubcommands_ContainsExpected)
{
    auto cmds = knownSubcommands();
    auto has  = [&](const std::string& s) {
        return std::find(cmds.begin(), cmds.end(), s) != cmds.end();
    };
    EXPECT_TRUE(has("status"));
    EXPECT_TRUE(has("load-module"));
    EXPECT_TRUE(has("unload-module"));
    EXPECT_TRUE(has("reload-module"));
    EXPECT_TRUE(has("list-modules"));
    EXPECT_TRUE(has("module-info"));
    EXPECT_TRUE(has("info"));
    EXPECT_TRUE(has("call"));
    EXPECT_TRUE(has("watch"));
    EXPECT_TRUE(has("stats"));
    EXPECT_TRUE(has("stop"));
    EXPECT_TRUE(has("daemon"));
    EXPECT_TRUE(has("issue-token"));
    EXPECT_TRUE(has("revoke-token"));
    EXPECT_TRUE(has("list-tokens"));
}

// ── Connection Error Handling ────────────────────────────────────────────────

TEST_F(CommandTest, LoadModule_NoDaemon_ReturnsExit2)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("load-module", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 2);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

// ── load-module ──────────────────────────────────────────────────────────────

TEST_F(CommandTest, LoadModule_Success)
{
    mockClient.loadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}, {"version", "0.1.0"},
        {"dependencies_loaded", nlohmann::json::array({"store"})}
    };

    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastLoadedModule, "waku");

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
    EXPECT_EQ(doc["module"].get<std::string>(), "waku");
}

TEST_F(CommandTest, LoadModule_NotFound)
{
    mockClient.loadModuleResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_NOT_FOUND"},
        {"message", "Module 'nonexistent' not found."}
    };

    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"nonexistent"});
        EXPECT_EQ(exitCode, 3);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "MODULE_NOT_FOUND");
}

TEST_F(CommandTest, LoadModule_MissingArg)
{
    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "INVALID_ARGS");
}

// ── unload-module ────────────────────────────────────────────────────────────

TEST_F(CommandTest, UnloadModule_Success)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}
    };

    auto cmd = createCommand("unload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastUnloadedModule, "waku");
}

// The cascade is the default — a bare `unload-module X` must take X's
// dependents down with it, since leaving them bound to an unloaded provider
// is the more surprising outcome.
TEST_F(CommandTest, UnloadModule_CascadesToDependentsByDefault)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}
    };
    mockClient.lastUnloadWithDependents = false;  // prove the command sets it

    auto cmd = createCommand("unload-module", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"waku"}), 0);
    });

    EXPECT_TRUE(mockClient.lastUnloadWithDependents);
}

TEST_F(CommandTest, UnloadModule_NoDependentsOptsOutOfCascade)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}
    };

    auto cmd = createCommand("unload-module", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"waku", "--no-dependents"}), 0);
    });

    EXPECT_EQ(mockClient.lastUnloadedModule, "waku");
    EXPECT_FALSE(mockClient.lastUnloadWithDependents);
}

// A cascade that quietly stops three other modules has to be reported.
TEST_F(CommandTest, UnloadModule_Human_ReportsUnloadedDependents)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"},
        {"dependents_unloaded", LogosList{"chat", "delivery"}}
    };

    output.setHumanMode(true);
    auto cmd = createCommand("unload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"waku"}), 0);
    });

    EXPECT_NE(out.find("chat"), std::string::npos);
    EXPECT_NE(out.find("delivery"), std::string::npos);
}

// ── reload-module ────────────────────────────────────────────────────────────

TEST_F(CommandTest, ReloadModule_Success)
{
    mockClient.reloadModuleResult = LogosMap{
        {"action", "reload"}, {"module", "chat"}, {"version", "0.2.0"},
        {"status", "loaded"}, {"pid", 51203}
    };

    auto cmd = createCommand("reload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastReloadedModule, "chat");
}

TEST_F(CommandTest, ReloadModule_Error)
{
    mockClient.reloadModuleResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_LOAD_FAILED"},
        {"message", "Module failed to start."}
    };

    auto cmd = createCommand("reload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 3);
    });
}

// ── list-modules ─────────────────────────────────────────────────────────────

TEST_F(CommandTest, ListModules_All)
{
    mockClient.listModulesResult = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"status", "loaded"}},
        LogosMap{{"name", "chat"}, {"status", "not_loaded"}}
    });

    auto cmd = createCommand("list-modules", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastListFilter, "all");
}

TEST_F(CommandTest, ListModules_LoadedFilter)
{
    mockClient.listModulesResult = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"status", "loaded"}}
    });

    auto cmd = createCommand("list-modules", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"--loaded"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastListFilter, "loaded");
}

// ── module-info / info ───────────────────────────────────────────────────────

TEST_F(CommandTest, ModuleInfo_Success)
{
    mockClient.moduleInfoResult = LogosMap{
        {"name", "chat"}, {"version", "0.2.0"}, {"status", "loaded"},
        {"pid", 23457}, {"uptime_seconds", 8040}
    };

    auto cmd = createCommand("module-info", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastInfoModule, "chat");
}

TEST_F(CommandTest, InfoAlias_SameAsModuleInfo)
{
    mockClient.moduleInfoResult = LogosMap{
        {"name", "chat"}, {"version", "0.2.0"}, {"status", "loaded"}
    };

    auto cmd = createCommand("info", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastInfoModule, "chat");
}

TEST_F(CommandTest, ModuleInfo_NotFound)
{
    mockClient.moduleInfoResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_NOT_FOUND"},
        {"message", "Module 'nonexistent' not found."}
    };

    auto cmd = createCommand("module-info", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"nonexistent"});
        EXPECT_EQ(exitCode, 3);
    });
}

// ── call ─────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Call_Success)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "ok"}, {"module", "chat"}, {"method", "send_message"},
        {"result", "message sent (id: msg_123)"}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "send_message", "hello"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastCallModule, "chat");
    EXPECT_EQ(mockClient.lastCallMethod, "send_message");
    EXPECT_EQ(mockClient.lastCallArgs.size(), 1u);
}

TEST_F(CommandTest, Call_VerboseSyntax)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "ok"}, {"module", "chat"}, {"method", "send_message"}
    };

    auto cmd = createCommand("module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "method", "send_message", "hello"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastCallModule, "chat");
    EXPECT_EQ(mockClient.lastCallMethod, "send_message");
}

TEST_F(CommandTest, Call_MethodNotFound)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "error"}, {"code", "METHOD_NOT_FOUND"},
        {"message", "Method 'bad' not found on module 'chat'."}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "bad"});
        EXPECT_EQ(exitCode, 4);
    });
}

// core_service now answers an unknown method with the module's real method list
// (it asks the module, because no provider distinguishes "no such method" from
// "returned null" on the wire). Both output modes have to surface it.
TEST_F(CommandTest, Call_MethodNotFound_JsonCarriesAvailableMethods)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "error"}, {"code", "METHOD_NOT_FOUND"},
        {"message", "Method 'bad' not found on module 'chat'."},
        {"available_methods", LogosList::array({"send_message", "get_history"})}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"chat", "bad"}), 4);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "METHOD_NOT_FOUND");
    EXPECT_EQ(doc["available_methods"],
              nlohmann::json::array({"send_message", "get_history"}));
}

TEST_F(CommandTest, Call_MethodNotFound_HumanListsAvailableMethods)
{
    // Human mode prints the message and nothing else, so the list has to be
    // folded into the message — and it goes to stderr, not stdout.
    //
    // setHumanMode(true), not Output{false}: the latter only declines to FORCE
    // JSON and then auto-detects from the TTY, which makes the test depend on
    // how the runner's stdout is wired (nix gives builds a pty, a plain pipe
    // does not) — the sibling OutputTest cases are TTY-dependent for exactly
    // this reason.
    Output humanOutput{false};
    humanOutput.setHumanMode(true);
    mockClient.callMethodResult = LogosMap{
        {"status", "error"}, {"code", "METHOD_NOT_FOUND"},
        {"message", "Method 'bad' not found on module 'chat'."},
        {"available_methods", LogosList::array({"send_message", "get_history"})}
    };

    auto cmd = createCommand("call", mockClient, humanOutput);
    std::stringstream buffer;
    auto oldBuf = std::cerr.rdbuf(buffer.rdbuf());
    EXPECT_EQ(cmd->execute({"chat", "bad"}), 4);
    std::cerr.rdbuf(oldBuf);

    const std::string err = buffer.str();
    EXPECT_NE(err.find("Method 'bad' not found on module 'chat'."), std::string::npos)
        << err;
    EXPECT_NE(err.find("Available methods: send_message, get_history"),
              std::string::npos) << err;
}

// The behaviour change, at the CLI layer: a null RESULT is a successful call.
// It used to be unreachable — core_service turned every null into
// METHOD_FAILED, because it read the value instead of the error channel.
TEST_F(CommandTest, Call_NullResultIsSuccess)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "ok"}, {"module", "chat"}, {"method", "maybe_get"},
        {"result", nullptr}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"chat", "maybe_get"}), 0);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
    ASSERT_TRUE(doc.contains("result"));
    EXPECT_TRUE(doc["result"].is_null());
}

TEST_F(CommandTest, Call_ModuleNotLoaded)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_NOT_LOADED"},
        {"message", "Module 'delivery' is not loaded."}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"delivery", "send_package"});
        EXPECT_EQ(exitCode, 3);
    });
}

TEST_F(CommandTest, Call_MissingArgs)
{
    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });
}

// ── call: argument type coercion ─────────────────────────────────────────────
// Args reach the daemon as native JSON types. Regression guard: a decimal
// like "1.25" must NOT be truncated to int 1 (std::stoi accepts a numeric
// prefix), so the whole string has to be consumed for a numeric type.

TEST_F(CommandTest, Call_CoercesDecimalArgsToDouble)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}, {"result", 4.0}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"calc", "addDoubles", "1.25", "2.75"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[0].get<double>(), 1.25);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[1].get<double>(), 2.75);
}

TEST_F(CommandTest, Call_CoercesIntegerArgsToInt)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"calc", "addInts", "3", "-4"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int>(), 3);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[1].get<int>(), -4);
}

// LIDL `int`/`uint` are 64-bit, so an argument has to survive the full range.
// This used std::stoi (32 bits): every integer outside int32 threw out_of_range,
// was swallowed, and came back out of std::stod as a DOUBLE. Under 2^53 that is
// exact and looks correct, which is why it went unnoticed — above it the value
// is silently rounded. Found by probing a live provider:
//   echoUint 9007199254740993 -> 9007199254740992
TEST_F(CommandTest, Call_KeepsSixtyFourBitIntegerArgsExact)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f",
                                "4294967296",            // > int32max
                                "9007199254740993",      // > 2^53: double rounds it
                                "9223372036854775807",   // int64max
                                "-9223372036854775808"}), // int64min
                  0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 4u);
    for (const auto& a : mockClient.lastCallArgs)
        EXPECT_TRUE(a.is_number_integer()) << "arg reached the daemon as " << a.dump();
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int64_t>(), 4294967296LL);
    EXPECT_EQ(mockClient.lastCallArgs[1].get<int64_t>(), 9007199254740993LL);
    EXPECT_EQ(mockClient.lastCallArgs[2].get<int64_t>(), 9223372036854775807LL);
    EXPECT_EQ(mockClient.lastCallArgs[3].get<int64_t>(), INT64_MIN);
}

// The band above int64max is still a valid `uint`. stoull is tried only for a
// non-negative literal — stoull("-1") wraps to uint64max rather than failing.
TEST_F(CommandTest, Call_CoercesAboveInt64MaxToUnsigned)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "18446744073709551615", "-1"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_unsigned());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<uint64_t>(), 18446744073709551615ULL);
    // Still signed, NOT wrapped.
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[1].get<int64_t>(), -1);
}

// Beyond uint64 there is no integer type left; a numeric literal that fits
// neither still goes as a double rather than a string.
TEST_F(CommandTest, Call_FallsBackToDoubleBeyondUint64)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "99999999999999999999999"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_float());
}

TEST_F(CommandTest, Call_CoercesMixedArgTypes)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "hi", "3.0", "true", "5"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 4u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "hi");
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[1].get<double>(), 3.0);
    EXPECT_TRUE(mockClient.lastCallArgs[2].is_boolean());
    EXPECT_TRUE(mockClient.lastCallArgs[2].get<bool>());
    EXPECT_TRUE(mockClient.lastCallArgs[3].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[3].get<int>(), 5);
}

TEST_F(CommandTest, Call_TrimsWhitespaceForNumericCoercion)
{
    // @file params commonly arrive with a trailing newline; "123\n" must still
    // coerce to a number rather than fall through to a string.
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "123\n", " 1.5 "}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int>(), 123);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[1].get<double>(), 1.5);
}

// A `0x`-hex argument is a STRING, all the way through the command.
//
// arg_coerce's own suite pins the inference; this pins the wiring -- that `call`
// really routes a positional argument through it, so the address the user typed
// is what the RPC carries. It was a double (7.9250088148318908e+47) and the
// module answered {"result": null, "status": "ok"} to every method that names
// an account.
TEST_F(CommandTest, Call_HexArgumentReachesTheModuleAsAString)
{
    const std::string addr = "0x8ad0Fcf71D6FBD060BAfd45f5155b1e52d3591C5";
    mockClient.callMethodResult = LogosMap{{"status", "ok"}, {"result", true}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"keystore_module", "has_address", addr, "0x1F", "inf"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 3u);
    for (const auto& a : mockClient.lastCallArgs)
        EXPECT_TRUE(a.is_string()) << "arg reached the daemon as " << a.dump();
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), addr);
    EXPECT_EQ(mockClient.lastCallArgs[1].get<std::string>(), "0x1F");
    EXPECT_EQ(mockClient.lastCallArgs[2].get<std::string>(), "inf");
}

// ── call: json: / str: argument prefixes ─────────────────────────────────────
// Scalar coercion can only produce bool/int/double/string, so `json:<value>`
// opts into JSON parsing (list / map / nested), `json:@file` parses file
// contents, and `str:<text>` forces a literal string past all coercion. These
// are the two explicit escapes that make containers and every literal string
// expressible from the command line.

TEST_F(CommandTest, Call_JsonPrefixParsesList)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "echoList", "json:[1,2,3]"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_array());
    EXPECT_EQ(mockClient.lastCallArgs[0], (LogosList{1, 2, 3}));
    // Integers inside the list must stay integers, not degrade to double.
    EXPECT_TRUE(mockClient.lastCallArgs[0][0].is_number_integer());
}

TEST_F(CommandTest, Call_JsonPrefixParsesMap)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "echoMap", R"(json:{"k":"v","n":42})"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_object());
    EXPECT_EQ(mockClient.lastCallArgs[0].value("k", std::string{}), "v");
    EXPECT_EQ(mockClient.lastCallArgs[0].value("n", 0), 42);
}

TEST_F(CommandTest, Call_JsonPrefixParsesNestedAndScalars)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        // json: also expresses a bare scalar unambiguously (a real int/bool),
        // and a nested value the default path could never produce.
        EXPECT_EQ(cmd->execute({"m", "f", "json:42", "json:true",
                                R"(json:{"a":[1,{"b":2}]})"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 3u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int>(), 42);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_boolean());
    EXPECT_TRUE(mockClient.lastCallArgs[1].get<bool>());
    EXPECT_EQ(mockClient.lastCallArgs[2]["a"][1]["b"].get<int>(), 2);
}

TEST_F(CommandTest, Call_JsonPrefixFromFile)
{
    const std::string path = testing::TempDir() + "logosctl_call_json_arg.json";
    { std::ofstream f(path); f << "[10, 20, 30]"; }
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "echoList", "json:@" + path}), 0);
    });
    std::remove(path.c_str());
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_array());
    EXPECT_EQ(mockClient.lastCallArgs[0], (LogosList{10, 20, 30}));
}

TEST_F(CommandTest, Call_JsonPrefixMalformedErrors)
{
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 0;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "json:hello"});
    });
    EXPECT_EQ(exitCode, 1);
    // Rejected before the RPC — the method was never dialed.
    EXPECT_NE(mockClient.lastCallMethod, "f");
}

TEST_F(CommandTest, Call_JsonPrefixMissingFileErrors)
{
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 0;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "json:@/no/such/logosctl/file.json"});
    });
    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(mockClient.lastCallMethod, "f");
}

TEST_F(CommandTest, Call_StrPrefixForcesLiteralString)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        // Every value the default path would otherwise reinterpret — a
        // json:-looking string, a number, a bool, an @file reference — stays a
        // literal string under str:, with the prefix stripped.
        EXPECT_EQ(cmd->execute({"m", "f", "str:json:x", "str:42",
                                "str:true", "str:@config.json"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 4u);
    for (const auto& a : mockClient.lastCallArgs)
        EXPECT_TRUE(a.is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "json:x");
    EXPECT_EQ(mockClient.lastCallArgs[1].get<std::string>(), "42");
    EXPECT_EQ(mockClient.lastCallArgs[2].get<std::string>(), "true");
    EXPECT_EQ(mockClient.lastCallArgs[3].get<std::string>(), "@config.json");
}

TEST_F(CommandTest, Call_StrPrefixExpressesEmptyString)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "str:"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "");
}

TEST_F(CommandTest, Call_EmptyFileYieldsEmptyStringNotError)
{
    // A readable-but-empty @file is a successful read of "", distinct from an
    // unreadable file (which errors). Regression guard for resolveFileParam's
    // couldn't-open vs read-but-empty distinction.
    const std::string path = testing::TempDir() + "logosctl_call_empty_arg";
    { std::ofstream f(path); }  // create empty
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 1;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "@" + path});
    });
    std::remove(path.c_str());
    EXPECT_EQ(exitCode, 0);
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "");
}

TEST_F(CommandTest, Call_MissingFileErrors)
{
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 0;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "@/no/such/logosctl/file.txt"});
    });
    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(mockClient.lastCallMethod, "f");
}

// ── stats ────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Stats_Success)
{
    mockClient.moduleStatsResult = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"pid", 23456}, {"cpu_percent", 2.1}, {"memory_mb", 48.3}},
        LogosMap{{"name", "chat"}, {"pid", 23457}, {"cpu_percent", 0.4}, {"memory_mb", 22.1}}
    });

    auto cmd = createCommand("stats", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    nlohmann::json doc = parseJson(out);
    ASSERT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 2u);
}

// ── watch ────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Watch_MissingArgs)
{
    auto cmd = createCommand("watch", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });
}

TEST_F(CommandTest, Watch_ParsesModuleAndEventName)
{
    auto cmd = createCommand("watch", mockClient, output);
    captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "--event", "message"});
        EXPECT_EQ(exitCode, 3);  // watchShouldSucceed=false => WATCH_FAILED
    });

    EXPECT_EQ(mockClient.lastWatchModule,    "chat");
    EXPECT_EQ(mockClient.lastWatchEventName, "message");
}

TEST_F(CommandTest, Watch_ParsesModuleOnly)
{
    auto cmd = createCommand("watch", mockClient, output);
    captureOutput([&]() {
        cmd->execute({"waku"});
    });

    EXPECT_EQ(mockClient.lastWatchModule,    "waku");
    EXPECT_EQ(mockClient.lastWatchEventName, "");
}

TEST_F(CommandTest, Watch_ModuleNotLoaded_ReturnsExit3)
{
    auto cmd = createCommand("watch", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"missing"});
        EXPECT_EQ(exitCode, 3);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "WATCH_FAILED");
    EXPECT_NE(doc["message"].get<std::string>().find("'missing'"), std::string::npos);
}

TEST_F(CommandTest, Watch_NoDaemon_ReturnsExit2)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("watch", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "--event", "message"});
        EXPECT_EQ(exitCode, 2);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

// ── stop ─────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Stop_Success)
{
    mockClient.shutdownResult = LogosMap{
        {"status", "ok"}, {"message", "Daemon shutting down."}
    };

    auto cmd = createCommand("stop", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_TRUE(mockClient.shutdownCalled);

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
}

TEST_F(CommandTest, Stop_NoDaemon)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("stop", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 2);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

// ── stop: telling a live daemon from a session someone forgot to clean up ────
//
// `stop` reports success when the shutdown RPC produces no reply but the
// daemon's pid is gone -- which is the normal outcome, since the daemon is
// being asked to die mid-sentence. That inference is only sound about a pid
// that was alive when we asked. A session whose daemon died last week has a
// state.json naming a dead pid and a dial spec beside it that still
// "connects" (a LocalSocket client never checks that anyone is listening), so
// without the guard these three become one indistinguishable "ok".

namespace {

// Beyond every platform's pid ceiling (macOS 99998, Linux's default 4194304,
// and not a multiple of 4, which Windows pids always are), so kill(pid, 0)
// answers ESRCH rather than "some unrelated process".
constexpr long long kPidThatCannotExist = 2147483646LL;

// Write a session that says "instance <id> is running as pid <pid>", as a
// booted daemon would, and a client dial spec pointing at the same instance.
void seedSession(const std::string& instanceId, long long pid)
{
    DaemonRuntimeState rs;
    rs.instanceId = instanceId;
    rs.pid        = pid;
    rs.startedAt  = currentUtcIso8601();
    ASSERT_TRUE(DaemonRuntimeStateFile::write(rs));

    ClientState cs;
    cs.fileOk        = true;
    cs.schemaVersion = kClientStateSchemaVersion;
    cs.tokenFile     = "auto.json";
    cs.instanceId    = instanceId;
    ClientStateFile::setOverride(cs);
}

} // namespace

TEST_F(CommandTest, Stop_StaleSession_ReportsNoDaemonWithoutCallingShutdown)
{
    seedSession("deadbeef1234", kPidThatCannotExist);
    if (::testing::Test::HasFatalFailure()) return;

    auto cmd = createCommand("stop", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 2);
    });

    EXPECT_FALSE(mockClient.shutdownCalled)
        << "there is nothing to shut down -- the RPC must not be attempted, or "
           "its silence becomes evidence that it worked";
    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

TEST_F(CommandTest, Stop_LiveSession_StillStops)
{
    // The control. A guard that refuses every session would satisfy the test
    // above and break the command; this pins the other side of the line.
    seedSession("deadbeef1234", static_cast<long long>(getpid()));
    if (::testing::Test::HasFatalFailure()) return;

    mockClient.shutdownResult = LogosMap{
        {"status", "ok"}, {"message", "Daemon shutting down."}
    };

    auto cmd = createCommand("stop", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 0);
    });

    EXPECT_TRUE(mockClient.shutdownCalled);
    EXPECT_EQ(parseJson(out)["status"].get<std::string>(), "ok");
}

TEST_F(CommandTest, Stop_StaleSessionForAnotherInstance_DoesNotBlockARemoteStop)
{
    // A remote client can have a co-resident daemon's leftovers in its own
    // session directory. Those describe someone else's daemon -- the instance
    // ids differ -- and must not stop it from stopping the one it dials.
    DaemonRuntimeState rs;
    rs.instanceId = "aaaaaaaaaaaa";       // the dead co-resident daemon
    rs.pid        = kPidThatCannotExist;
    rs.startedAt  = currentUtcIso8601();
    ASSERT_TRUE(DaemonRuntimeStateFile::write(rs));

    ClientState cs;                        // ...dialing a different one
    cs.fileOk        = true;
    cs.schemaVersion = kClientStateSchemaVersion;
    cs.tokenFile     = "remote.json";
    cs.instanceId    = "bbbbbbbbbbbb";
    ClientStateFile::setOverride(cs);

    mockClient.shutdownResult = LogosMap{
        {"status", "ok"}, {"message", "Daemon shutting down."}
    };

    auto cmd = createCommand("stop", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 0);
    });

    EXPECT_TRUE(mockClient.shutdownCalled);
    EXPECT_EQ(parseJson(out)["status"].get<std::string>(), "ok");
}

// ── every command that opens with an RPC refuses a dead session ─────────────
//
// The guard above is not a `stop` detail. Any command whose first act is an
// RPC has the same problem, for the same reason: the LocalSocket "connect"
// against a session nobody is serving succeeds, the request goes nowhere, and
// QtRO has nothing to report -- so the command sits until Timeout(20000) in
// logos-protocol's cpp/logos_mode.h expires, twenty seconds after the session
// directory on disk could have said "that pid is gone".
//
// So the guard lives in Command::ensureConnected(), which is the one door all
// of them go through, and these sweep both sides of it: nothing is dialled
// when the session is provably dead, and nothing is refused when it is not.

namespace {

struct RpcCommand {
    const char*              command;   // createCommand() name
    std::vector<std::string> args;
    const char*              typed;     // how a person would type it
};

// One entry per ensureConnected() call site in src/client/commands, package's
// six included. A command added there without a line here is a command with
// no coverage for this, which is how the other thirteen got missed the first
// time round.
const std::vector<RpcCommand> kRpcCommands{
    {"call",          {"chat", "send", "hi"}, "call chat send hi"},
    {"catalog",       {"ls"},                 "catalog ls"},
    {"key",           {"ls"},                 "key ls"},
    {"list-modules",  {},                     "module ls"},
    {"load-module",   {"chat"},               "module load chat"},
    {"module-info",   {"chat"},               "module show chat"},
    {"package",       {"install", "chat"},    "package install chat"},
    {"package",       {"ls"},                 "package ls"},
    {"package",       {"show", "chat"},       "package show chat"},
    {"package",       {"deps", "chat"},       "package deps chat"},
    {"package",       {"search", "chat"},     "package search chat"},
    {"package",       {"download", "chat"},   "package download chat"},
    {"reload-module", {"chat"},               "module reload chat"},
    {"stats",         {},                     "stats"},
    {"stop",          {},                     "stop"},
    {"unload-module", {"chat"},               "module unload chat"},
    {"watch",         {"chat"},               "watch chat"},
};

// Well-formed replies for every RPC, so that a command which gets further than
// it should fails on the assertion below rather than on an exception thrown
// while reading a default-constructed (null) result. Only the stale-session
// cases need this -- they are the ones where reaching an RPC is the bug.
void primeReplies(MockClient& c)
{
    c.loadModuleResult   = LogosMap{{"status", "ok"}, {"version", "1.0.0"}};
    c.unloadModuleResult = LogosMap{{"status", "ok"}};
    c.reloadModuleResult = LogosMap{{"status", "ok"}};
    c.moduleInfoResult   = LogosMap{{"status", "ok"}, {"name", "chat"}};
    c.callMethodResult   = LogosMap{{"status", "ok"}, {"result", nullptr}};
    c.shutdownResult     = LogosMap{{"status", "ok"}, {"message", "bye"}};
    c.planPackageResult  = LogosMap{{"status", "ok"}, {"changes", LogosList::array()}};
    c.applyPackageResult = LogosMap{{"status", "ok"}};
    c.downloadResult     = LogosMap{{"status", "ok"}, {"result", LogosMap{{"path", "/p.lgx"}}}};
    c.listModulesResult  = LogosList::array();
    c.moduleStatsResult  = LogosList::array();
}

} // namespace

TEST_F(CommandTest, EveryRpcCommand_StaleSession_FailsWithoutDialling)
{
    for (const RpcCommand& c : kRpcCommands) {
        SCOPED_TRACE(c.typed);

        seedSession("deadbeef1234", kPidThatCannotExist);
        if (::testing::Test::HasFatalFailure()) return;

        MockClient mock;          // fresh counters per command
        mock.shouldConnect = true;  // connecting is not the thing that saves us
        primeReplies(mock);

        auto cmd = createCommand(c.command, mock, output);
        ASSERT_NE(cmd, nullptr);

        const std::string printed = captureOutput([&]() {
            EXPECT_EQ(cmd->execute(c.args), 2);
        });

        EXPECT_EQ(mock.connectAttempts, 0)
            << "dialled a session whose daemon is known to be gone";
        EXPECT_EQ(mock.rpcCalls, 0)
            << "sent an RPC into a dead session -- this is the twenty-second "
               "wait the guard exists to prevent";

        const nlohmann::json doc = parseJson(printed);
        EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
        EXPECT_NE(doc["message"].get<std::string>().find("stale state file"),
                  std::string::npos)
            << "the message has to say WHY, or the operator cannot find the "
               "session directory to clean up: " << doc["message"];
    }
}

// The other side of the line, three ways. A guard that refused everything
// would pass the sweep above and break the CLI, so each of these seeds a
// session the guard must stay silent about and checks that the command still
// gets as far as dialling. `shouldConnect = false` stops it there: what is
// under test is whether the guard let it try, not what the daemon replies.
TEST_F(CommandTest, EveryRpcCommand_LiveSession_StillDials)
{
    for (const RpcCommand& c : kRpcCommands) {
        SCOPED_TRACE(c.typed);

        seedSession("deadbeef1234", static_cast<long long>(getpid()));
        if (::testing::Test::HasFatalFailure()) return;

        MockClient mock;
        mock.shouldConnect = false;   // stop at the door; the dial is the point

        auto cmd = createCommand(c.command, mock, output);
        ASSERT_NE(cmd, nullptr);
        captureOutput([&]() { EXPECT_EQ(cmd->execute(c.args), 2); });

        EXPECT_EQ(mock.connectAttempts, 1)
            << "refused a session whose daemon is alive";
    }
}

TEST_F(CommandTest, EveryRpcCommand_StaleSessionForAnotherInstance_StillDials)
{
    // A remote client's session directory can hold a co-resident daemon's
    // leftovers. They describe someone else's dead daemon; the one at the far
    // end of its TCP connection is fine, and has no local pid to check.
    for (const RpcCommand& c : kRpcCommands) {
        SCOPED_TRACE(c.typed);

        DaemonRuntimeState rs;
        rs.instanceId = "aaaaaaaaaaaa";           // the dead co-resident daemon
        rs.pid        = kPidThatCannotExist;
        rs.startedAt  = currentUtcIso8601();
        ASSERT_TRUE(DaemonRuntimeStateFile::write(rs));

        ClientState cs;                          // ...dialing a different one
        cs.fileOk        = true;
        cs.schemaVersion = kClientStateSchemaVersion;
        cs.tokenFile     = "remote.json";
        cs.instanceId    = "bbbbbbbbbbbb";
        ClientStateFile::setOverride(cs);

        MockClient mock;
        mock.shouldConnect = false;

        auto cmd = createCommand(c.command, mock, output);
        ASSERT_NE(cmd, nullptr);
        captureOutput([&]() { EXPECT_EQ(cmd->execute(c.args), 2); });

        EXPECT_EQ(mock.connectAttempts, 1)
            << "a co-resident daemon's leftovers blocked a remote client";
    }
}

TEST_F(CommandTest, EveryRpcCommand_NoStateFileAtAll_StillDials)
{
    // The ordinary remote case: a hand-written dial spec, no daemon/state.json
    // anywhere, and therefore no local pid to have an opinion about. "No
    // evidence of a dead daemon" must not be read as "the daemon is dead".
    for (const RpcCommand& c : kRpcCommands) {
        SCOPED_TRACE(c.typed);

        ClientState cs;
        cs.fileOk        = true;
        cs.schemaVersion = kClientStateSchemaVersion;
        cs.tokenFile     = "remote.json";
        ClientStateFile::setOverride(cs);        // no instance_id: a TCP dial

        MockClient mock;
        mock.shouldConnect = false;

        auto cmd = createCommand(c.command, mock, output);
        ASSERT_NE(cmd, nullptr);
        captureOutput([&]() { EXPECT_EQ(cmd->execute(c.args), 2); });

        EXPECT_EQ(mock.connectAttempts, 1)
            << "refused a client with no local daemon to check";
    }
}

// ── an unanswered query is not an empty answer ──────────────────────────────
//
// `module ls` and `stats` were the only two commands that reported a failed
// RPC as data. Both returned LogosList::array() when nothing replied, so
// against a daemon that was not running they printed `[]` and exited 0 -- the
// one outcome a script cannot argue with. Everything else in the client
// returns an error envelope for the same failure.

TEST_F(CommandTest, ListModules_UnansweredRpc_IsNotAnEmptyList)
{
    mockClient.listModulesResult = std::nullopt;   // the RPC produced no reply

    auto cmd = createCommand("list-modules", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 2)
            << "exit 0 here means \"the daemon answered, nothing is loaded\"";
    });

    const nlohmann::json doc = parseJson(printed);
    EXPECT_EQ(doc["status"].get<std::string>(), "error");
    EXPECT_EQ(doc["code"].get<std::string>(), "DAEMON_UNREACHABLE");
}

TEST_F(CommandTest, ListModules_AnsweredWithNothing_IsStillSuccess)
{
    // The control: an empty list is a perfectly good answer, and must not be
    // turned into an error by the check above.
    mockClient.listModulesResult = LogosList::array();

    auto cmd = createCommand("list-modules", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 0);
    });

    EXPECT_TRUE(parseJson(printed).is_array());
    EXPECT_TRUE(parseJson(printed).empty());
}

TEST_F(CommandTest, Stats_UnansweredRpc_IsNotAnEmptyList)
{
    mockClient.moduleStatsResult = std::nullopt;

    auto cmd = createCommand("stats", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 2);
    });

    const nlohmann::json doc = parseJson(printed);
    EXPECT_EQ(doc["status"].get<std::string>(), "error");
    EXPECT_EQ(doc["code"].get<std::string>(), "DAEMON_UNREACHABLE");
}

TEST_F(CommandTest, Stats_AnsweredWithNothing_IsStillSuccess)
{
    mockClient.moduleStatsResult = LogosList::array();

    auto cmd = createCommand("stats", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 0);
    });

    EXPECT_TRUE(parseJson(printed).is_array());
}

// `status` had the same shape of bug by a different route: RpcClient::getStatus
// synthesises a not_running report when nothing replies, and that report has a
// "daemon" key, so it reached the success branch and exited 0.
TEST_F(CommandTest, Status_UnansweredRpc_ReportsNotRunningAndExitsNonZero)
{
    // A live session, so `status` gets past "not_configured" and past the
    // stale-session guard and actually reaches the RPC.
    seedSession("deadbeef1234", static_cast<long long>(getpid()));
    if (::testing::Test::HasFatalFailure()) return;

    mockClient.statusResult = LogosMap{
        {"daemon",    LogosMap{{"status", "not_running"}, {"version", "1.0.0"}}},
        {"modules",   LogosList::array()},
        {"rpc_error", "core_service not reachable"},
    };

    auto cmd = createCommand("status", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 1)
            << "exit 0 reads as \"the daemon is fine\" to anything checking the "
               "code rather than the text";
    });

    const nlohmann::json doc = parseJson(printed);
    EXPECT_EQ(doc["daemon"]["status"].get<std::string>(), "not_running");
}

TEST_F(CommandTest, Status_LiveDaemon_StillExitsZero)
{
    seedSession("deadbeef1234", static_cast<long long>(getpid()));
    if (::testing::Test::HasFatalFailure()) return;

    mockClient.statusResult = LogosMap{
        {"daemon",  LogosMap{{"status", "running"}, {"pid", 4242}}},
        {"modules", LogosList::array()},
    };

    auto cmd = createCommand("status", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 0);
    });

    EXPECT_EQ(parseJson(printed)["daemon"]["status"].get<std::string>(), "running");
}

// ── status: same evidence, reported as a status rather than an error ────────

TEST_F(CommandTest, Status_StaleSession_ReportsNotRunningWithoutDialling)
{
    seedSession("deadbeef1234", kPidThatCannotExist);
    if (::testing::Test::HasFatalFailure()) return;

    auto cmd = createCommand("status", mockClient, output);
    const std::string printed = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({}), 1);
    });

    EXPECT_EQ(mockClient.connectAttempts, 0);
    EXPECT_EQ(mockClient.rpcCalls, 0);

    const nlohmann::json doc = parseJson(printed);
    EXPECT_EQ(doc["daemon"]["status"].get<std::string>(), "not_running");
    EXPECT_EQ(doc["daemon"]["pid"].get<long long>(), kPidThatCannotExist)
        << "naming the pid is what lets an operator confirm it really is gone";
}

TEST_F(CommandTest, Status_StaleSessionForAnotherInstance_StillDials)
{
    DaemonRuntimeState rs;
    rs.instanceId = "aaaaaaaaaaaa";
    rs.pid        = kPidThatCannotExist;
    rs.startedAt  = currentUtcIso8601();
    ASSERT_TRUE(DaemonRuntimeStateFile::write(rs));

    ClientState cs;
    cs.fileOk        = true;
    cs.schemaVersion = kClientStateSchemaVersion;
    cs.tokenFile     = "remote.json";
    cs.instanceId    = "bbbbbbbbbbbb";
    ClientStateFile::setOverride(cs);

    mockClient.shouldConnect = false;

    auto cmd = createCommand("status", mockClient, output);
    captureOutput([&]() { EXPECT_EQ(cmd->execute({}), 1); });

    EXPECT_EQ(mockClient.connectAttempts, 1)
        << "a co-resident daemon's leftovers decided a remote client's status";
}

// ── package search ──────────────────────────────────────────────────────────

// Search shows the latest version and a count of the rest: the full list made
// every row wrap, which is what the column was supposed to prevent.
TEST_F(CommandTest, PackageSearch_ShowsLatestVersionAndCountsTheRest)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "ok"},
        {"result", LogosList::array({
            LogosMap{
                {"name", "storage_module"},
                {"category", "storage"},
                {"description", "Persistent storage"},
                {"versions", LogosList::array({
                    LogosMap{{"manifest", LogosMap{{"version", "2.0.0"}}}},
                    LogosMap{{"manifest", LogosMap{{"version", "1.5.0"}}}},
                    // Different artifacts for the same release must not make
                    // the user-facing version list misleadingly repetitive.
                    LogosMap{{"manifest", LogosMap{{"version", "2.0.0"}}}},
                })},
            },
        })},
    };
    Output humanOutput;
    humanOutput.setHumanMode(true);

    auto cmd = createCommand("package", mockClient, humanOutput);
    const std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"search", "storage"}), 0);
    });

    EXPECT_EQ(mockClient.lastCallModule, "package_downloader");
    EXPECT_EQ(mockClient.lastCallMethod, "getCatalog");
    EXPECT_NE(out.find("VERSION"), std::string::npos);
    // Two distinct releases from three catalog artifacts -> "+1", not "+2".
    EXPECT_NE(out.find("2.0.0 (+1)"), std::string::npos);
    EXPECT_EQ(out.find("1.5.0"), std::string::npos);
}

// The other half of the split: `show` is where every version is named, and it
// answers for catalog-only packages rather than refusing them as uninstalled.
TEST_F(CommandTest, PackageShow_ListsEveryVersionForACatalogOnlyPackage)
{
    mockClient.callMethodResultByMethod["getInstalledPackages"] =
        LogosMap{{"status", "ok"}, {"result", LogosList::array()}};
    mockClient.callMethodResultByMethod["getCatalog"] = LogosMap{
        {"status", "ok"},
        {"result", LogosList::array({
            LogosMap{
                {"name", "storage_module"},
                {"category", "storage"},
                {"description", "Persistent storage"},
                {"versions", LogosList::array({
                    LogosMap{{"manifest", LogosMap{{"version", "2.0.0"}}}},
                    LogosMap{{"manifest", LogosMap{{"version", "1.5.0"}}}},
                    LogosMap{{"manifest", LogosMap{{"version", "2.0.0"}}}},
                })},
            },
        })},
    };
    Output humanOutput;
    humanOutput.setHumanMode(true);

    auto cmd = createCommand("package", mockClient, humanOutput);
    const std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"show", "storage_module"}), 0);
    });

    EXPECT_NE(out.find("available:"), std::string::npos);
    EXPECT_NE(out.find("2.0.0, 1.5.0"), std::string::npos);
    EXPECT_NE(out.find("installed:"), std::string::npos);
}

TEST_F(CommandTest, PackageShow_FailsWhenNeitherInstalledNorInAnyCatalog)
{
    mockClient.callMethodResultByMethod["getInstalledPackages"] =
        LogosMap{{"status", "ok"}, {"result", LogosList::array()}};
    mockClient.callMethodResultByMethod["getCatalog"] =
        LogosMap{{"status", "ok"}, {"result", LogosList::array()}};

    auto cmd = createCommand("package", mockClient, output);
    const std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"show", "nope_module"}), 1);
    });

    EXPECT_NE(out.find("PACKAGE_NOT_FOUND"), std::string::npos);
}

// ---------------------------------------------------------------------------
// package download
//
// These exist because `-o` was parsed and then thrown away — the option was
// accepted, the file went to $TMPDIR, and nothing anywhere said so. There was
// no unit coverage of PackageCommand at all, which is why it survived.
// ---------------------------------------------------------------------------

TEST_F(CommandTest, PackageDownload_PassesOutputDirectoryThrough)
{
    mockClient.downloadResult = LogosMap{
        {"status", "ok"},
        {"result", LogosMap{{"name", "storage_module"}, {"path", "/out/storage_module.lgx"}}}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() {
        int exitCode = cmd->execute({"download", "storage_module", "-o", "/out"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastDownloadName, "storage_module");
    EXPECT_EQ(mockClient.lastDownloadOpts.value("output", std::string{}), "/out");
}

// A relative -o has to become absolute before it leaves this process: the
// daemon does the move, and its working directory is not ours -- for a
// detached daemon it is wherever it happened to be started.
TEST_F(CommandTest, PackageDownload_ResolvesRelativeOutputAgainstOurCwd)
{
    mockClient.downloadResult = LogosMap{
        {"status", "ok"}, {"result", LogosMap{{"path", "/x/p.lgx"}}}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() { cmd->execute({"download", "pkg", "-o", "pkgs"}); });

    const std::string sent = mockClient.lastDownloadOpts.value("output", std::string{});
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent, (std::filesystem::current_path() / "pkgs").string())
        << "a relative -o must be resolved against the client's cwd, not sent raw";
}

// No -o means "the session's cache", which only the daemon knows the path of.
// Sending an empty string is how it is told to use that default.
TEST_F(CommandTest, PackageDownload_NoOutputDirLeavesTheChoiceToTheDaemon)
{
    mockClient.downloadResult = LogosMap{
        {"status", "ok"}, {"result", LogosMap{{"path", "/cache/downloads/p.lgx"}}}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() { cmd->execute({"download", "pkg"}); });

    EXPECT_EQ(mockClient.lastDownloadOpts.value("output", std::string("unset")), "");
}

TEST_F(CommandTest, PackageDownload_ReportsFailure)
{
    mockClient.downloadResult = LogosMap{
        {"status", "error"}, {"code", "DOWNLOAD_FAILED"}, {"message", "no such package"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"download", "nope"});
        EXPECT_EQ(exitCode, 1);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "DOWNLOAD_FAILED");
}

// ── package failure reporting ───────────────────────────────────────────────
//
// A real install failure was reported as "install failed at step '?': " with
// nothing after the colon. The daemon had said why; the client dropped it,
// because package_ops returns two different error shapes and this only read
// one. The reason is the entire value of the message.

TEST_F(CommandTest, PackageMutate_ReportsTheReasonFromEitherErrorShape)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","blockchain_module"},
                                               {"action","install"},
                                               {"toVersion","0.2.1"}}})},
        {"affected_loaded", LogosList::array()}};

    // The shape produced before the step chain starts: code + message.
    mockClient.applyPackageResult = LogosMap{
        {"status", "error"},
        {"code", "DOWNLOAD_FAILED"},
        {"message", "package_downloader did not respond"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"install", "blockchain_module", "-y"});
        EXPECT_EQ(exitCode, 1);
    });

    nlohmann::json doc = parseJson(out);
    const std::string msg = doc["message"].get<std::string>();
    EXPECT_NE(msg.find("package_downloader did not respond"), std::string::npos)
        << "the daemon's reason must survive to the user; got: " << msg;
    EXPECT_EQ(msg.find("step '?'"), std::string::npos)
        << "an unknown step should be omitted, not printed as '?': " << msg;
}

TEST_F(CommandTest, PackageMutate_KeepsTheStepWhenTheDaemonReportsOne)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","storage_module"},
                                               {"action","install"}}})},
        {"affected_loaded", LogosList::array()}};

    // The shape package_ops' own `fail()` produces: failed_step + error.
    mockClient.applyPackageResult = LogosMap{
        {"status", "error"},
        {"failed_step", "confirm"},
        {"error", "package_manager rejected the install"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() { cmd->execute({"install", "storage_module", "-y"}); });

    const std::string msg = parseJson(out)["message"].get<std::string>();
    EXPECT_NE(msg.find("confirm"), std::string::npos) << msg;
    EXPECT_NE(msg.find("package_manager rejected the install"), std::string::npos) << msg;
}

// Never print a bare colon with nothing after it: if both shapes are empty we
// still owe the reader a sentence.
TEST_F(CommandTest, PackageMutate_SaysSoWhenNoReasonWasReported)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","x"},{"action","install"}}})},
        {"affected_loaded", LogosList::array()}};
    mockClient.applyPackageResult = LogosMap{{"status", "error"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() { cmd->execute({"install", "x", "-y"}); });

    const std::string msg = parseJson(out)["message"].get<std::string>();
    EXPECT_NE(msg.find("no reason reported"), std::string::npos) << msg;
}

// ── local .lgx files ────────────────────────────────────────────────────────
//
// install/upgrade take a package off disk as readily as out of a catalog. The
// two resolve by completely different rules, and the daemon plans one way or
// the other -- so what the client hands over has to be unambiguously one or
// the other before it leaves here.

// A path is a path. Reading it as a catalog name sent it to the resolver,
// which came back "no candidate matches './foo.lgx'" -- a package-not-found
// error for a file that was sitting right there.
TEST_F(CommandTest, PackageInstall_PositionalLgxPathIsReadAsAFile)
{
    const std::string path = testing::TempDir() + "logosctl_pkg_positional.lgx";
    { std::ofstream f(path); f << "not really an archive"; }

    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","m"},{"action","install"}}})},
        {"affected_loaded", LogosList::array()}};
    mockClient.applyPackageResult = LogosMap{{"status", "ok"}};

    auto cmd = createCommand("package", mockClient, output);
    int exitCode = 1;
    captureOutput([&]() { exitCode = cmd->execute({"install", path, "-y"}); });
    std::remove(path.c_str());

    EXPECT_EQ(exitCode, 0);
    // It travels as a local file, not as a name.
    EXPECT_TRUE(mockClient.lastPackageNames.empty());
    const auto& files = mockClient.lastPackageOpts["localFiles"];
    ASSERT_TRUE(files.is_array());
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].get<std::string>(),
              std::filesystem::absolute(path).string());
}

TEST_F(CommandTest, PackageInstall_MissingLgxPathIsReportedAsAMissingFile)
{
    auto cmd = createCommand("package", mockClient, output);
    int exitCode = 0;
    std::string out = captureOutput([&]() {
        exitCode = cmd->execute({"install", "./no_such_package.lgx", "-y"});
    });

    EXPECT_EQ(exitCode, 1);
    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "INVALID_ARGS");
    EXPECT_NE(doc["message"].get<std::string>().find("No such .lgx file"),
              std::string::npos) << doc.dump();
    // Nothing reached the daemon, so nothing was half-installed.
    EXPECT_FALSE(mockClient.applyPackageCalled);
}

// The plan is either/or, so a request carrying both used to install the files
// and silently drop the names.
TEST_F(CommandTest, PackageInstall_RefusesToMixCatalogNamesWithLocalFiles)
{
    const std::string path = testing::TempDir() + "logosctl_pkg_mixed.lgx";
    { std::ofstream f(path); }

    auto cmd = createCommand("package", mockClient, output);
    int exitCode = 0;
    std::string out = captureOutput([&]() {
        exitCode = cmd->execute({"install", "storage_module", path, "-y"});
    });
    std::remove(path.c_str());

    EXPECT_EQ(exitCode, 1);
    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "INVALID_ARGS");
    EXPECT_NE(doc["message"].get<std::string>().find("Cannot mix"),
              std::string::npos) << doc.dump();
    EXPECT_FALSE(mockClient.applyPackageCalled);
}

// --file plus an empty --dir: the emptiness test used to look at the combined
// list, so the directory contributing nothing went unreported.
TEST_F(CommandTest, PackageInstall_EmptyDirIsReportedEvenAlongsideAFile)
{
    const std::string dir = testing::TempDir() + "logosctl_pkg_empty_dir";
    std::filesystem::create_directories(dir);
    const std::string path = testing::TempDir() + "logosctl_pkg_with_dir.lgx";
    { std::ofstream f(path); }

    auto cmd = createCommand("package", mockClient, output);
    int exitCode = 0;
    std::string out = captureOutput([&]() {
        exitCode = cmd->execute({"install", "--file", path, "--dir", dir, "-y"});
    });
    std::remove(path.c_str());
    std::filesystem::remove(dir);

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(parseJson(out)["message"].get<std::string>().find("No .lgx files found"),
              std::string::npos) << out;
    EXPECT_FALSE(mockClient.applyPackageCalled);
}

// `remove` names an installed package. Both flags were accepted and then
// ignored by the daemon, so the command reported "already up to date" and
// removed nothing.
TEST_F(CommandTest, PackageRemove_RejectsFileAndDirRatherThanIgnoringThem)
{
    const std::string path = testing::TempDir() + "logosctl_pkg_remove.lgx";
    { std::ofstream f(path); }

    auto cmd = createCommand("package", mockClient, output);
    int exitCode = 0;
    std::string out = captureOutput([&]() {
        exitCode = cmd->execute({"remove", "--file", path, "-y"});
    });
    std::remove(path.c_str());

    EXPECT_EQ(exitCode, 1);
    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "INVALID_ARGS");
    EXPECT_NE(doc["message"].get<std::string>().find("--file / --dir apply to install"),
              std::string::npos) << doc.dump();
    EXPECT_FALSE(mockClient.applyPackageCalled);
}

// Removal is by name even when the name happens to end in `.lgx`: there is no
// file to read, so the path stays a name and the daemon reports it as not
// installed.
TEST_F(CommandTest, PackageRemove_KeepsAnLgxArgumentAsAName)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "error"}, {"code", "NOT_INSTALLED"},
        {"message", "Package './thing.lgx' is not installed."}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() { cmd->execute({"remove", "./thing.lgx", "-y"}); });

    ASSERT_EQ(mockClient.lastPackageNames.size(), 1u);
    EXPECT_EQ(mockClient.lastPackageNames[0].get<std::string>(), "./thing.lgx");
    EXPECT_TRUE(mockClient.lastPackageOpts["localFiles"].empty());
}

// ── `package deps` row rendering ─────────────────────────────────────────────
//
// `optional` reaches the CLI on every dependency row (package-manager-module
// #67). `--json` passes it through untouched; the human table has to say it
// out loud, or an absent optional dependency is indistinguishable from a
// broken install — the same row, the same `not_installed`.

TEST(PackageDepsRow, MarksAnOptionalDependency)
{
    const std::string row = PackageCommand::formatDependencyRow(
        LogosMap{{"name", "nice_to_have"}, {"status", "not_installed"},
                 {"version", ""}, {"installType", ""}, {"optional", true}});
    EXPECT_NE(row.find("not_installed (optional)"), std::string::npos) << row;
}

TEST(PackageDepsRow, LeavesARequiredDependencyUnmarked)
{
    // The identical row but for the flag: absent means required, so nothing is
    // appended and every pre-existing row renders byte-for-byte as before.
    const std::string row = PackageCommand::formatDependencyRow(
        LogosMap{{"name", "needed"}, {"status", "not_installed"},
                 {"version", ""}, {"installType", ""}});
    EXPECT_NE(row.find("not_installed"), std::string::npos) << row;
    EXPECT_EQ(row.find("optional"), std::string::npos) << row;
}

TEST(PackageDepsRow, ShowsADashWhenNothingIsInstalled)
{
    // `"version": ""` is PRESENT but empty, so the `-` default on the lookup
    // never fired and the column rendered blank. Pinned as the whole row, so
    // this is also where the column layout is anchored.
    const std::string row = PackageCommand::formatDependencyRow(
        LogosMap{{"name", "libbar"}, {"status", "not_installed"},
                 {"version", ""}, {"installType", ""}});
    EXPECT_EQ(row, "libbar                       -            not_installed");
}

TEST(PackageDepsRow, MarksAnOptionalDependencyThatIsInstalled)
{
    // The flag is per-EDGE, not a verdict about the package, so it shows on a
    // satisfied row too — that is what tells a user this one is safe to remove.
    const std::string row = PackageCommand::formatDependencyRow(
        LogosMap{{"name", "extra"}, {"status", "installed"},
                 {"version", "1.0.0"}, {"installType", "user"},
                 {"optional", true}});
    EXPECT_NE(row.find("installed (optional)"), std::string::npos) << row;
    EXPECT_NE(row.find("1.0.0"), std::string::npos) << row;
}

TEST(PackageDepsRow, ADependentRowFallsBackToInstallType)
{
    // resolveFlatDependents emits no `status`. Unchanged by the above, and the
    // reverse walk never marks optionality in the first place.
    const std::string row = PackageCommand::formatDependencyRow(
        LogosMap{{"name", "parent"}, {"version", "2.0.0"},
                 {"installType", "user"}});
    EXPECT_NE(row.find("user"), std::string::npos) << row;
    EXPECT_EQ(row.find("optional"), std::string::npos) << row;
}
