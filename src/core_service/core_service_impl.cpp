#include "core_service_impl.h"
#include "package_ops.h"
#include "call_envelope.h"
#include "logos_core.h"
#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_call_error.h>
#include <logos_json_convert.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <unistd.h>
#include <unordered_set>

void CoreServiceImpl::onInit(LogosAPI* api)
{
    m_api = api;
}

// ---------------------------------------------------------------------------
// Helpers to convert C API char** to std::vector<std::string>
// ---------------------------------------------------------------------------

std::vector<std::string> CoreServiceImpl::getKnownModuleNames()
{
    // logos_core_* hands back memory allocated with new[], never malloc:
    // module_manager.cpp's toNullTerminatedArray does `new char*[]` plus a
    // `new char[]` per element, and getModulesInfoCStr / ProcessStats::
    // getModuleStats each `new char[]`. delete[] is therefore the only correct
    // deallocator for every one of them — free() here was undefined behaviour
    // that happened not to crash. (logos-basecamp's CoreModuleManager.cpp
    // carries the same note; the two hosts drain the same C API.)
    std::vector<std::string> result;
    char** modules = logos_core_get_known_modules();
    if (modules) {
        for (int i = 0; modules[i] != nullptr; ++i) {
            result.emplace_back(modules[i]);
            delete[] modules[i];
        }
        delete[] modules;
    }
    return result;
}

std::vector<std::string> CoreServiceImpl::getLoadedModuleNames()
{
    std::vector<std::string> result;
    char** modules = logos_core_get_loaded_modules();
    if (modules) {
        for (int i = 0; modules[i] != nullptr; ++i) {
            result.emplace_back(modules[i]);
            delete[] modules[i];
        }
        delete[] modules;
    }
    return result;
}

static bool containsName(const std::vector<std::string>& v, const std::string& name)
{
    return std::find(v.begin(), v.end(), name) != v.end();
}

// Uptime in seconds for a module entry from getModulesInfo(): now - loaded_at,
// only meaningful for loaded modules. Returns -1 when the module isn't loaded
// or carries no load timestamp, so callers can skip emitting uptime_seconds.
// The daemon stamps loaded_at with the same wall clock, so this stays consistent.
static int64_t uptimeSecondsFor(const nlohmann::json& entry)
{
    if (!entry.value("loaded", false))
        return -1;
    int64_t loadedAt = entry.value("loaded_at", int64_t{0});
    if (loadedAt <= 0)
        return -1;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t up = now - loadedAt;
    return up < 0 ? 0 : up;
}

nlohmann::json CoreServiceImpl::getModulesInfo()
{
    nlohmann::json info = nlohmann::json::array();
    char* json = logos_core_get_modules_info();
    if (json) {
        nlohmann::json parsed = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_array())
            info = std::move(parsed);
        delete[] json;
    }
    return info;
}

std::string CoreServiceImpl::getModuleVersion(const std::string& name)
{
    for (const auto& entry : getModulesInfo()) {
        if (entry.value("name", std::string{}) == name) {
            const auto& meta = entry["metadata"];
            if (meta.is_object())
                return meta.value("version", std::string{});
            return {};
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

StdLogosResult CoreServiceImpl::loadModule(const std::string& name)
{
    // Snapshot the loaded set before the call so we can report which
    // *dependencies* this load brought up as a side effect.
    // logos_core_load_module(..., LOGOS_LOAD_REQUIRED_AND_OPTIONAL) resolves and
    // loads the target's transitive dependency closure, so any module that
    // wasn't loaded before but is loaded after — other than the target
    // itself — was auto-resolved on its behalf.
    std::vector<std::string> before = getLoadedModuleNames();

    bool ok = logos_core_load_module(name.c_str(), LOGOS_LOAD_REQUIRED_AND_OPTIONAL);
    if (!ok) {
        LogosMap errResult;
        errResult["status"] = "error";
        errResult["code"] = "MODULE_LOAD_FAILED";
        errResult["message"] = "Failed to load module '" + name + "'.";

        auto known = getKnownModuleNames();
        LogosList names = LogosList::array();
        for (const auto& n : known)
            names.push_back(n);
        errResult["known_modules"] = names;
        return {false, errResult, "Failed to load module '" + name + "'."};
    }

    std::unordered_set<std::string> beforeSet(before.begin(), before.end());
    LogosList dependenciesLoaded = LogosList::array();
    for (const auto& loaded : getLoadedModuleNames()) {
        if (loaded != name && beforeSet.find(loaded) == beforeSet.end())
            dependenciesLoaded.push_back(loaded);
    }

    LogosMap result;
    result["status"] = "ok";
    result["module"] = name;
    result["version"] = getModuleVersion(name);
    // Still "what this load brought up", which now includes the optional
    // dependencies that were installed — they are auto-resolved on the
    // target's behalf like any other.
    result["dependencies_loaded"] = dependenciesLoaded;
    // ...and the other half of that answer. Loading optionals never fails, so
    // without this a caller cannot tell an optional dependency that is not
    // installed from one it simply did not think to ask about. Only present
    // when something WAS left out: a key that is always there says nothing.
    if (char* report = logos_core_optional_load_report(name.c_str())) {
        const std::string json(report);
        delete[] report;
        if (json != "[]" && !json.empty())
            result["optional_skipped"] = LogosList::parse(json);
    }
    return {true, result};
}

StdLogosResult CoreServiceImpl::unloadModule(const std::string& name, bool withDependents)
{
    // Snapshot before the call so we can report which *dependents* came down
    // as a side effect, mirroring how loadModule reports dependencies_loaded.
    std::vector<std::string> before = getLoadedModuleNames();

    logos_core_unload_module(name.c_str(), withDependents);

    auto loaded = getLoadedModuleNames();
    if (containsName(loaded, name)) {
        LogosMap errResult;
        errResult["status"] = "error";
        errResult["code"] = "MODULE_NOT_LOADED";
        errResult["message"] = "Module '" + name + "' is not loaded.";
        return {false, errResult, "Module '" + name + "' is not loaded."};
    }

    std::unordered_set<std::string> afterSet(loaded.begin(), loaded.end());
    LogosList dependentsUnloaded = LogosList::array();
    for (const auto& wasLoaded : before) {
        if (wasLoaded != name && afterSet.find(wasLoaded) == afterSet.end())
            dependentsUnloaded.push_back(wasLoaded);
    }

    LogosMap result;
    result["status"] = "ok";
    result["module"] = name;
    result["dependents_unloaded"] = dependentsUnloaded;
    return {true, result};
}

namespace {

// Translate the wire shape into package_ops::Options.
package_ops::Options toOptions(const LogosMap& opts)
{
    package_ops::Options o;
    o.withDeps       = opts.value("withDeps", true);
    o.withDependents = opts.value("withDependents", true);
    o.version        = opts.value("version", std::string{});
    o.rootHash       = opts.value("rootHash", std::string{});
    o.catalog        = opts.value("catalog", std::string{});
    if (opts.contains("localFiles") && opts["localFiles"].is_array()) {
        for (const auto& f : opts["localFiles"])
            o.localFiles.push_back(f.get<std::string>());
    }
    return o;
}

std::optional<package_ops::Op> toOp(const std::string& op)
{
    if (op == "install") return package_ops::Op::Install;
    if (op == "upgrade") return package_ops::Op::Upgrade;
    if (op == "remove")  return package_ops::Op::Remove;
    return std::nullopt;
}

std::vector<std::string> toNames(const LogosList& names)
{
    std::vector<std::string> out;
    if (names.is_array())
        for (const auto& n : names) out.push_back(n.get<std::string>());
    return out;
}

} // namespace

LogosMap CoreServiceImpl::planPackageOperation(const std::string& op,
                                               const LogosList& names,
                                               const LogosMap& opts)
{
    auto parsed = toOp(op);
    if (!parsed)
        return LogosMap{{"status", "error"}, {"code", "INVALID_ARGS"},
                        {"message", "Unknown package operation: " + op}};
    return package_ops::plan(m_api, *parsed, toNames(names), toOptions(opts));
}

LogosMap CoreServiceImpl::applyPackageOperation(const std::string& op,
                                                const LogosList& names,
                                                const LogosMap& opts)
{
    auto parsed = toOp(op);
    if (!parsed)
        return LogosMap{{"status", "error"}, {"code", "INVALID_ARGS"},
                        {"message", "Unknown package operation: " + op}};
    return package_ops::apply(m_api, *parsed, toNames(names), toOptions(opts));
}

LogosMap CoreServiceImpl::downloadPackage(const std::string& name,
                                          const LogosMap& opts)
{
    const std::string dest = opts.is_object()
        ? opts.value("output", std::string{})
        : std::string{};
    return package_ops::download(m_api, name, toOptions(opts), dest);
}

LogosMap CoreServiceImpl::refreshModules()
{
    logos_core_refresh_modules();

    LogosList known = LogosList::array();
    for (const auto& n : getKnownModuleNames())
        known.push_back(n);

    LogosMap result;
    result["status"] = "ok";
    result["known_modules"] = known;
    return result;
}

StdLogosResult CoreServiceImpl::reloadModule(const std::string& name)
{
    LogosMap result;
    result["action"] = "reload";
    result["module"] = name;

    auto loaded = getLoadedModuleNames();
    const bool wasLoaded = containsName(loaded, name);
    std::string previousStatus = wasLoaded ? "loaded" : "not_loaded";
    result["previous_status"] = previousStatus;

    if (wasLoaded) {
        logos_core_unload_module(name.c_str(), false);
    }

    bool ok = logos_core_load_module(name.c_str(), LOGOS_LOAD_REQUIRED_AND_OPTIONAL);
    if (!ok) {
        // Non-destructive on failure: if it was running, try to bring it back
        // (the user asked to reload, not to take it down) and report whether
        // the prior instance was restored.
        if (wasLoaded) {
            const bool restored = logos_core_load_module(name.c_str(), LOGOS_LOAD_REQUIRED_AND_OPTIONAL);
            result["status"]   = "error";
            result["error"]    = restored
                ? "reload failed; previous instance restored"
                : "reload failed; module is now unloaded";
            result["restored"] = restored;
            return {false, result,
                    restored ? "reload failed; previous instance restored"
                             : "reload failed; module is now unloaded"};
        }
        result["status"] = "error";
        result["error"] = "module failed to start";
        return {false, result, "module failed to start"};
    }

    result["status"] = "loaded";
    result["version"] = getModuleVersion(name);
    return {true, result};
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

LogosList CoreServiceImpl::listModules(const std::string& filter)
{
    LogosList modules = LogosList::array();

    // Single call to the runtime gives name + loaded + metadata for every
    // known module; version comes straight from the embedded metadata.
    for (const auto& entry : getModulesInfo()) {
        const bool loaded = entry.value("loaded", false);
        if (filter == "loaded" && !loaded)
            continue;

        LogosMap mod;
        mod["name"]    = entry.value("name", std::string{});
        mod["status"]  = loaded ? "loaded" : "not_loaded";

        const auto& meta = entry["metadata"];
        mod["version"] = meta.is_object() ? meta.value("version", std::string{})
                                          : std::string{};

        // Uptime is only meaningful for loaded modules.
        int64_t uptime = uptimeSecondsFor(entry);
        if (uptime >= 0)
            mod["uptime_seconds"] = uptime;

        modules.push_back(mod);
    }

    return modules;
}

LogosMap CoreServiceImpl::getStatus()
{
    LogosMap status;

    LogosMap daemon;
    daemon["status"] = "running";
    daemon["pid"] = static_cast<int64_t>(getpid());
    daemon["version"] = version();
    status["daemon"] = daemon;

    LogosList modules = listModules("all");
    status["modules"] = modules;

    int loadedCount = 0, crashed = 0, notLoaded = 0;
    for (const auto& v : modules) {
        std::string s = v.value("status", "");
        if (s == "loaded") loadedCount++;
        else if (s == "crashed") crashed++;
        else notLoaded++;
    }
    LogosMap summary;
    summary["loaded"] = loadedCount;
    summary["crashed"] = crashed;
    summary["not_loaded"] = notLoaded;
    status["modules_summary"] = summary;

    return status;
}

LogosMap CoreServiceImpl::getModuleInfo(const std::string& name)
{
    LogosMap info;

    // Find the module's entry in the runtime's modules-info dump.
    nlohmann::json entry;
    for (const auto& e : getModulesInfo()) {
        if (e.value("name", std::string{}) == name) { entry = e; break; }
    }
    if (entry.is_null()) {
        info["status"] = "error";
        info["code"] = "MODULE_NOT_FOUND";
        info["message"] = "Module '" + name + "' not found.";
        return info;
    }

    info["name"] = name;
    const auto& meta = entry["metadata"];
    info["version"] = meta.is_object() ? meta.value("version", std::string{})
                                       : std::string{};
    // Dependency graph comes from the same dump (direct edges).
    info["dependencies"] = entry.value("dependencies", nlohmann::json::array());
    info["dependents"]   = entry.value("dependents", nlohmann::json::array());

    if (entry.value("loaded", false)) {
        info["status"] = "loaded";

        int64_t uptime = uptimeSecondsFor(entry);
        if (uptime >= 0)
            info["uptime_seconds"] = uptime;

        if (m_api) {
            // Use the nlohmann::json overload — no QJson types needed here.
            //
            // These two are SHAPE checks, not the failure-from-null inference
            // that callModuleMethod carried: `methods`/`events` are optional
            // enrichment, and both a failed introspection and a provider that
            // answered something other than an array leave the field absent,
            // which is the same and correct outcome — there is nothing to
            // report either way. The error channel would let module-info say
            // WHY it has nothing, but that is a change to this envelope and
            // deliberately not made here.
            LogosAPIClient* moduleClient = m_api->getClient(QString::fromStdString(name));
            if (moduleClient) {
                nlohmann::json methods = moduleClient->invokeRemoteMethod(
                    name, "getPluginMethods", nlohmann::json::array());
                if (methods.is_array())
                    info["methods"] = methods;

                nlohmann::json events = moduleClient->invokeRemoteMethod(
                    name, "getPluginEvents", nlohmann::json::array());
                if (events.is_array())
                    info["events"] = events;
            }
        }
    } else {
        info["status"] = "not_loaded";
    }

    return info;
}

LogosList CoreServiceImpl::getModuleStats()
{
    LogosList stats = LogosList::array();
    char* json = logos_core_get_module_stats();
    if (json) {
        try {
            stats = nlohmann::json::parse(json);
        } catch (...) {}
        delete[] json;
    }
    return stats;
}

// ---------------------------------------------------------------------------
// Proxied call — takes the CallError-carrying SDK overload; no QJson needed
// ---------------------------------------------------------------------------

namespace {

// The DECLARED parameter types of one published method, or nullopt when the
// module did not describe them usably — which is why MethodInfo carries
// paramsPublished rather than inferring it from an empty list. "Takes no
// parameters" and "did not say" are different answers and the caller acts on
// only one of them.
std::optional<std::vector<std::string>> publishedParamTypes(const nlohmann::json& method)
{
    // Absent because the method takes none — the shape both provider flavours
    // emit for a zero-parameter method.
    auto params = method.find("parameters");
    if (params == method.end()) return std::vector<std::string>{};
    if (!params->is_array()) return std::nullopt;

    std::vector<std::string> types;
    for (const auto& p : *params) {
        auto t = p.is_object() ? p.find("type") : p.end();
        // A parameter whose type is missing or not a string leaves the WHOLE
        // list unusable: the positions have to line up with the arguments or
        // the wrong one gets named.
        if (t == p.end() || !t->is_string()) return std::nullopt;
        types.push_back(t->get<std::string>());
    }
    return types;
}

// The interface a module says it exposes, via its own getPluginMethods.
//
// Only ever consulted to resolve the ambiguities the wire genuinely cannot
// (see call_envelope.cpp), so the extra round-trip is paid on a null return and
// nowhere else. Returns empty when introspection itself failed — the caller
// must then not claim the method is missing, because it does not know.
std::vector<core_service::MethodInfo> exposedMethods(LogosAPIClient* client,
                                                     const std::string& module)
{
    std::vector<core_service::MethodInfo> out;
    logos::CallError err;
    const nlohmann::json methods = logos::qvariantToNlohmann(
        client->invokeRemoteMethod(QString::fromStdString(module),
                                   QStringLiteral("getPluginMethods"),
                                   QVariantList(), Timeout(), &err));
    if (!err.ok() || !methods.is_array()) return out;
    for (const auto& m : methods) {
        core_service::MethodInfo info;
        if (m.is_string()) {
            info.name = m.get<std::string>();   // a bare name says nothing about arguments
        } else if (m.is_object()) {
            auto n = m.find("name");
            if (n == m.end() || !n->is_string()) continue;
            info.name = n->get<std::string>();
            if (auto types = publishedParamTypes(m)) {
                info.paramTypes      = std::move(*types);
                info.paramsPublished = true;
            }
        } else {
            continue;
        }
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace

StdLogosResult CoreServiceImpl::callModuleMethod(const std::string& module,
                                                 const std::string& method,
                                                 const LogosList& args)
{
    LogosMap result;

    if (!m_api) {
        result["status"]  = "error";
        result["code"]    = "INTERNAL_ERROR";
        result["message"] = "core_service not initialized.";
        return {false, result, "core_service not initialized."};
    }

    // LOADED, not merely known -- the same line watchModuleEvents draws below.
    // Without it an absent module cost a 20s acquire that raced the CLI client's
    // own 20s deadline, so the error code was a coin flip.
    //
    // core_service is exempt (the daemon publishes it itself, so it is never in
    // the loaded set). A module still warming up IS in the set -- liblogos marks
    // loaded before publish -- so it keeps its full acquire budget.
    if (module != name() && !containsName(getLoadedModuleNames(), module)) {
        result["status"]  = "error";
        result["code"]    = "MODULE_NOT_LOADED";
        result["message"] = "Module '" + module + "' is not loaded. Load it with: logosctl module load " + module;
        return {false, result, "Module '" + module + "' is not loaded."};
    }

    LogosAPIClient* moduleClient = m_api->getClient(QString::fromStdString(module));
    if (!moduleClient) {
        // Unreachable (getClient never returns null) but we deref it below.
        // INTERNAL_ERROR, not MODULE_NOT_LOADED: the gate above said it IS loaded.
        result["status"]  = "error";
        result["code"]    = "INTERNAL_ERROR";
        result["message"] = "Could not obtain a client for loaded module '" + module + "'.";
        return {false, result, "Could not obtain a client for loaded module '" + module + "'."};
    }

    // Take the overload that carries an error OUT-CHANNEL, and do the
    // json<->QVariant conversion here.
    //
    // The convenient nlohmann::json overload cannot be used: it forwards to this
    // very call with the logos::CallError* argument simply dropped
    // (logos_api_client.cpp), leaving its caller nothing but the value. That is
    // why this function used to read failure out of a null RESULT — and why
    // that was wrong: lp_invoke branches on callErr.ok() and NEVER on the value,
    // so a failed call and a method returning null were already distinct
    // everywhere else on this surface. Only here did they collapse.
    logos::CallError err;
    const QVariant qret = moduleClient->invokeRemoteMethod(
        QString::fromStdString(module), QString::fromStdString(method),
        logos::nlohmannArgsToQVariantList(args), Timeout(), &err);
    const nlohmann::json ret = logos::qvariantToNlohmann(qret);

    result = core_service::callEnvelope(
        module, method, ret, args,
        core_service::CallFailure{err.code, err.message, err.origin},
        [&]() { return exposedMethods(moduleClient, module); });

    const bool ok = result.value("status", std::string{}) == "ok";
    if (!ok)
        return {false, result, result.value("message", std::string{})};
    return {true, result};
}

// ---------------------------------------------------------------------------
// Event forwarding — uses the nlohmann::json onEvent overload
// ---------------------------------------------------------------------------

bool CoreServiceImpl::watchModuleEvents(const std::string& module,
                                        const std::string& eventName)
{
    if (!m_api)
        return false;

    LogosAPIClient* moduleClient = m_api->getClient(QString::fromStdString(module));
    if (!moduleClient)
        return false;

    // The line the contract is drawn on: LOADED, not merely known.
    //
    // A module that is not loaded may never be, so refusing is the useful
    // answer -- deferring would park `watch` on a subscription with no future
    // and report success for it. A typo lands here too, and gets the same
    // answer rather than a worse one.
    //
    // Past this point the subscription MUST succeed, and it does by
    // construction: nothing below can fail for a module that is loaded.
    // onEventWhenAvailable / whenObjectAvailable answer 0 only for arguments
    // they refuse (an empty name, a null callback), none of which are
    // reachable here, and neither one touches the transport on this thread.
    const auto loaded = getLoadedModuleNames();
    if (std::find(loaded.begin(), loaded.end(), module) == loaded.end())
        return false;

    auto forward = [this, module](const QString& event, const QVariantList& data) {
        nlohmann::json forwardData = nlohmann::json::array();
        forwardData.push_back(module);
        forwardData.push_back(event.toStdString());
        for (const QVariant& v : data)
            forwardData.push_back(logos::qvariantToNlohmann(v));
        if (emitEvent)
            emitEvent("module_event", forwardData.dump());
    };

    // Deferred on purpose, and this is what makes "loaded => succeeds" true.
    //
    // requestObject() + onEvent() is ONE-SHOT, and LogosAPIConsumer::
    // requestObject refuses outright while the module's registry socket has no
    // listener yet -- which is exactly the state a LOADED module is in for the
    // stretch after `load-module` RETURNS: the host reports it loaded once the
    // plugin is in, and the module publishes its object afterwards. Cold, that
    // gap is seconds. Because nothing retried, `watch` refused a module the
    // host had just called loaded, and since the failure went unchecked the
    // rest of the script ran green while observing nothing.
    //
    // onEventWhenAvailable holds the subscription instead, arms it the moment
    // the object appears, and re-arms it across a reconnect -- so the gap is
    // covered without this call ever blocking or failing.
    //
    // The wildcard form (`watch <module>` with no --event) goes through the
    // SAME call: an empty eventName means "every event on this object", which
    // is what LogosObject::onEvent has always understood it to mean. It used to
    // need a detour through whenObjectAvailable() + requestObject() + onEvent()
    // because onEventWhenAvailable refused an empty name -- one guard rejecting
    // three unrelated arguments at once. logos-protocol#74 removed that, so
    // both forms are one line and neither can be silently the odd one out.
    return moduleClient->onEventWhenAvailable(QString::fromStdString(module),
                                              QString::fromStdString(eventName),
                                              forward) != 0;
}

// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

// Milliseconds between answering a `shutdown` RPC and leaving the event loop.
//
// This is a courtesy margin, not the mechanism that gets the reply out --
// see the drain in shutdown() below. It exists so that anything the transport
// wants to do on its own schedule (heartbeats, a second in-flight call) still
// gets a turn. $LOGOSCTL_SHUTDOWN_GRACE_MS overrides it; the daemon-stop
// integration test pins it to 0, which is the hostile setting that used to
// lose the reply outright and must now be survivable.
static int shutdownGraceMs()
{
    static const int ms = []() {
        constexpr int kDefault = 200;
        const char* v = std::getenv("LOGOSCTL_SHUTDOWN_GRACE_MS");
        if (!v || !*v) return kDefault;
        char* end = nullptr;
        const long n = std::strtol(v, &end, 10);
        if (end == v || *end != '\0' || n < 0 || n > 60000) return kDefault;
        return static_cast<int>(n);
    }();
    return ms;
}

// Upper bound on the drain pass. processEvents() returns as soon as the queue
// is empty, so this is only reached if something keeps re-arming work; the
// point is that a busy daemon cannot turn "flush the reply" into "never exit".
static constexpr int kShutdownDrainMs = 2000;

LogosMap CoreServiceImpl::shutdown()
{
    LogosMap result;
    result["status"] = "ok";
    result["message"] = "Daemon shutting down.";

    // `result` is not on the wire yet. The transport serialises it *after*
    // this function returns and hands the bytes to the socket, which only
    // pushes them out when the event loop next services that socket's write
    // notifier. So whatever ends the event loop must run after that, or the
    // reply dies buffered inside a process that no longer exists.
    //
    // The previous shape -- a detached std::thread that slept 200ms and then
    // called QCoreApplication::quit() -- could not guarantee that, for two
    // reasons that compound:
    //
    //   * quit() is not a queued event. QCoreApplication::exit() reaches into
    //     the main thread's QThreadData, flags every event loop as exiting and
    //     interrupts the dispatcher. The loop then returns WITHOUT another
    //     pass, so a write notifier that had not fired yet never fires.
    //   * the sleep is wall clock and the flush is not. If the daemon's main
    //     thread was descheduled for longer than the grace period -- routine
    //     on a loaded CI runner -- the timer won.
    //
    // The client saw no transport error at all (QtRO reports none: the source
    // simply stopped talking), sat until its RPC deadline, and reported
    // RPC_FAILED for a shutdown that had in fact succeeded.
    //
    // A main-thread timer cannot fire until the loop is running again, and the
    // explicit drain below pushes the pending write out before the loop is
    // torn down. Neither depends on how long the main thread was away, nor on
    // whether a given platform's dispatcher happens to service socket
    // notifiers before timers.
    QTimer::singleShot(shutdownGraceMs(), qApp, []() {
        QCoreApplication::processEvents(QEventLoop::AllEvents, kShutdownDrainMs);
        QCoreApplication::quit();
    });

    return result;
}
