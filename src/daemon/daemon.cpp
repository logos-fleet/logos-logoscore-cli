#include "daemon.h"

#include <spdlog/spdlog.h>
#include "daemon_state.h"
#include "port_allocator.h"
#include "token_store.h"
#include "log_sink.h"
#include "package_bootstrap.h"
#include "../config.h"
#include "../paths.h"
#include "logos_core.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_call_error.h>
#include <logos_api_provider.h>
#include <logos_socket_paths.h>
#include <logos_transport_config.h>
#include <logos_transport_config_json.h>
#include <token_manager.h>
#include "../core_service/core_service_impl.h"

#include <QCoreApplication>
#include <QDir>
#include <QSocketNotifier>

#include <uuid.h>

#include <csignal>
#include <cstdint>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include "../platform_compat.h"
#include "../process_util.h"

#ifdef _WIN32
#include <process.h>   // getpid — mingw-w64 puts it here, not in unistd.h
#include <windows.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t g_shutdownRequested = 0;

#ifndef _WIN32
// Self-pipe: the handler write()s one byte (async-signal-safe), and a
// QSocketNotifier on the read end calls QCoreApplication::quit() in normal
// context — quit() itself is not async-signal-safe to call from a handler.
static int g_signalPipe[2] = {-1, -1};
#endif

// Ask the event loop to leave exec(), from wherever we are.
//
// On POSIX this runs in a signal handler and must stay async-signal-safe,
// which is what the self-pipe is for. On Windows there is no self-pipe here,
// and the reason is not stylistic: QEventDispatcherWin32 implements
// QSocketNotifier with WSAAsyncSelect, which requires a real SOCKET. Handing
// it a CRT pipe fd fails with WSAENOTSOCK, the dispatcher DISCARDS the return
// value, and the notifier simply never fires — a daemon that ignores Ctrl-C
// with no diagnostic at all. What Windows does instead is run the console
// control routine on a dedicated thread the OS spawns for it, so the
// constraint there is threading, not signal safety: post to the main thread.
void Daemon::signalHandler(int signal)
{
    (void)signal;
    g_shutdownRequested = 1;
#ifdef _WIN32
    if (QCoreApplication::instance())
        QMetaObject::invokeMethod(QCoreApplication::instance(), "quit",
                                  Qt::QueuedConnection);
#else
    if (g_signalPipe[1] != -1) {
        const char byte = 1;
        ssize_t n = ::write(g_signalPipe[1], &byte, 1);
        (void)n;
    }
#endif
}

#ifdef _WIN32
namespace {
// Ctrl-C, Ctrl-Break and the console-window/logoff/shutdown events all arrive
// here. Returning TRUE means "handled", which for CTRL_C_EVENT stops the
// default terminate-the-process behaviour and lets the shutdown below unwind
// normally (unlinking state.json, closing the QtRO pipes).
//
// For CTRL_CLOSE/LOGOFF/SHUTDOWN Windows grants a bounded grace period and
// then kills the process regardless — that is the OS contract, not something
// this handler can extend.
BOOL WINAPI consoleCtrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        Daemon::requestShutdown();
        return TRUE;
    default:
        return FALSE;
    }
}
}  // namespace

void Daemon::requestShutdown() { signalHandler(SIGTERM); }
#endif

void Daemon::setupSignalHandlers()
{
#ifdef _WIN32
    // Ctrl-C reaches a console process through this, not through signal():
    // the CRT's SIGINT emulation is itself implemented on top of it, and only
    // this form sees CTRL_CLOSE_EVENT (the window's X button).
    ::SetConsoleCtrlHandler(&consoleCtrlHandler, TRUE);

    // Windows has no way for one process to send SIGTERM to another, so a
    // raise() from inside this process is the only thing that can arrive here.
    // Registered anyway so that path shuts down the same way rather than
    // taking the CRT default of terminating outright.
    std::signal(SIGINT, &Daemon::signalHandler);
    std::signal(SIGTERM, &Daemon::signalHandler);
#else
    // Create the self-pipe and wire its read end to a QSocketNotifier that
    // performs the actual (non-async-signal-safe) quit() in normal context.
    if (::pipe(g_signalPipe) == 0) {
        auto* notifier = new QSocketNotifier(g_signalPipe[0],
                                             QSocketNotifier::Read,
                                             QCoreApplication::instance());
        QObject::connect(notifier, &QSocketNotifier::activated, []() {
            char buf[16];
            ssize_t n = ::read(g_signalPipe[0], buf, sizeof(buf));
            (void)n;  // just draining; one wake is enough
            QCoreApplication::quit();
        });
    }

    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

namespace {

// Materialize a per-module TransportInfo list into a LogosTransportSet
// that the SDK can hand to LogosAPI. For non-LocalSocket entries with
// `port == 0`, pre-allocate a fresh ephemeral port via PortAllocator
// (ask the kernel for a free TCP port, close the probe socket, hand
// the number to the listener). Without the pre-allocation the listener
// would race the kernel to bind and the actual port would only be
// known *after* the listener was up — too late to advertise in
// state.json.
//
// No "inheritance" here. Each module's transports are independent;
// nothing about core_service's set leaks into capability_module's.
// The CLI's `--module-transport` flags drive the input directly.
//
// Returns std::nullopt if any non-local listener fails to acquire a
// port. The caller is expected to abort daemon startup — silently
// advertising port=0 in state.json (the previous behaviour) is
// worse: clients pick up an unreachable endpoint and time out.
std::optional<LogosTransportSet> buildTransportSet(
    const std::vector<TransportInfo>& infos,
    const std::string& moduleName)
{
    LogosTransportSet out;
    for (const auto& src : infos) {
        TransportInfo eff = src;

        if (src.protocol != "local" && eff.port == 0) {
            eff.port = PortAllocator::allocateEphemeralTcp(eff.host);
            if (eff.port == 0) {
                fprintf(stderr,
                        "[%s] Failed to allocate ephemeral port for %s\n",
                        moduleName.c_str(), src.protocol.c_str());
                return std::nullopt;
            }
        }

        LogosTransportConfig c;
        if      (eff.protocol == "tcp")     c.protocol = LogosProtocol::Tcp;
        else if (eff.protocol == "tcp_ssl") c.protocol = LogosProtocol::TcpSsl;
        else                                c.protocol = LogosProtocol::LocalSocket;
        c.host       = eff.host;
        c.port       = eff.port;
        c.caFile     = eff.caFile;
        c.certFile   = eff.certFile;
        c.keyFile    = eff.keyFile;
        c.verifyPeer = eff.verifyPeer;
        c.codec      = (eff.codec == "cbor") ? LogosWireCodec::Cbor
                                              : LogosWireCodec::Json;
        out.push_back(std::move(c));
    }
    return out;
}

// Round-trip a LogosTransportSet back into the on-disk TransportInfo
// shape so we can advertise it under `modules.<name>.transports` in
// state.json. Reverse of buildTransportSet in the sense that the
// on-disk shape matches what clients then read.
std::vector<TransportInfo> toAdvertised(const LogosTransportSet& set)
{
    std::vector<TransportInfo> out;
    for (const auto& c : set) {
        TransportInfo t;
        switch (c.protocol) {
        case LogosProtocol::Tcp:         t.protocol = "tcp"; break;
        case LogosProtocol::TcpSsl:      t.protocol = "tcp_ssl"; break;
        case LogosProtocol::LocalSocket:
        default:                         t.protocol = "local"; break;
        }
        t.host = c.host;
        t.port = c.port;
        t.caFile = c.caFile;
        t.verifyPeer = c.verifyPeer;
        t.codec = (c.codec == LogosWireCodec::Cbor) ? "cbor" : "json";
        // certFile/keyFile intentionally NOT copied — they're
        // server-only secrets and don't belong in state.json.
        out.push_back(std::move(t));
    }
    return out;
}

// Load the bundled package modules and point package_manager at this
// session's directories. Best-effort throughout: a daemon that cannot manage
// packages is still fully usable for loading and calling modules, so nothing
// here is allowed to abort startup.
//
// The sequencing — which failure skips what — lives in package_bootstrap so it
// can be tested without a running core. This function is only the wiring from
// those hooks to logos_core and the live LogosAPI.
void bootstrapPackageModules(LogosAPI* api,
                             const std::string& bundledDir,
                             const std::string& signaturePolicy,
                             bool verbose)
{
    package_bootstrap::Hooks hooks;

    hooks.loadModule = [](const std::string& name) {
        return logos_core_load_module(name.c_str(), LOGOS_LOAD_REQUIRED_AND_OPTIONAL);
    };
    hooks.unloadModule = [](const std::string& name) {
        logos_core_unload_module(name.c_str(), /*with_dependents=*/true);
    };
    hooks.warn = [](const std::string& line) {
        fprintf(stderr, "%s\n", line.c_str());
    };
    if (verbose)
        hooks.note = [](const std::string& line) {
            fprintf(stderr, "%s\n", line.c_str());
        };

    // The CallError overload rather than the json one: a setter returns void,
    // so a dispatched call and a call that never reached the module are
    // indistinguishable in the return value. `err` is what tells them apart
    // ("object_unavailable" when the target object cannot be acquired), and
    // that distinction is what the signature-policy fail-closed rests on.
    hooks.configure = [api](const std::string& method,
                            const std::vector<std::string>& args) {
        LogosAPIClient* pm =
            api ? api->getClient(package_bootstrap::kPackageManager) : nullptr;
        if (!pm) return false;
        QVariantList qargs;
        for (const auto& a : args)
            qargs.push_back(QString::fromStdString(a));
        logos::CallError err;
        pm->invokeRemoteMethod(QString::fromLatin1(package_bootstrap::kPackageManager),
                               QString::fromStdString(method),
                               qargs, Timeout(), &err);
        return err.ok();
    };

    package_bootstrap::Dirs dirs;
    // bundledDir is <bin>/../modules; its plugins sibling is alongside it.
    if (!bundledDir.empty()) {
        dirs.embeddedModules   = bundledDir;
        dirs.embeddedUiPlugins =
            (std::filesystem::path(bundledDir).parent_path() / "plugins").string();
    }
    dirs.userModules   = Config::modulesDir();
    dirs.userUiPlugins = Config::pluginsDir();
    dirs.keyring       = Config::keyringDir();

    package_bootstrap::run(hooks, dirs, signaturePolicy);
}

} // namespace

int Daemon::start(int argc, char* argv[],
                  const DaemonConfig& cfg,
                  const std::string& configSource,
                  bool persistConfig,
                  bool verbose)
{
    const auto& modulesDirs      = cfg.modulesDirs;

    // Apply the session-directory redirects before anything asks Config for a
    // path. Defaults keep every directory inside the config dir, which is what
    // makes a session portable; an override is an explicit decision to move
    // one out (a shared keyring, a cache on a bigger disk, a modules tree
    // something else manages).
    // Everything gated on this is a logosctl feature; logoscore keeps the
    // behaviour it has today.
    const bool modern = (Config::flavor() == Config::Flavor::Modern);

    Config::setSessionDirOverride(Config::SessionDir::Modules, cfg.dirs.modules);
    Config::setSessionDirOverride(Config::SessionDir::Plugins, cfg.dirs.plugins);
    Config::setSessionDirOverride(Config::SessionDir::Keyring, cfg.dirs.keyring);
    Config::setSessionDirOverride(Config::SessionDir::Data,    cfg.dirs.data);
    Config::setSessionDirOverride(Config::SessionDir::Cache,   cfg.dirs.cache);
    Config::setSessionDirOverride(Config::SessionDir::Logs,    cfg.dirs.logs);

    // Start capturing before anything else runs, so the log holds the whole
    // boot -- including a failure during it, which is exactly when the log is
    // worth having. Non-fatal: a daemon that cannot write a log file is still
    // a working daemon.
    if (modern) {
        LogSink::Options lo;
        lo.enabled   = cfg.logging.enabled;
        lo.dir       = Config::logsDir();
        lo.file      = cfg.logging.file;
        lo.maxSizeMb = cfg.logging.maxSizeMb;
        lo.maxFiles  = cfg.logging.maxFiles;
        // Mirror whenever configured, pipe or terminal alike: a caller doing
        // `daemon start > out.log` is watching that pipe. Detached, the
        // original stdout is /dev/null, so this costs a discarded write.
        lo.console   = cfg.logging.console;
        if (cfg.logging.enabled && !LogSink::instance().start(lo)) {
            fprintf(stderr, "Warning: could not open the log file under %s; "
                            "continuing without file logging.\n",
                    lo.dir.c_str());
        }
    }
    const auto& moduleTransports = cfg.modules;
    // 1. Generate instance ID BEFORE core init, so logos_host inherits it
    std::random_device rd;
    std::mt19937 gen(rd());
    uuids::uuid_random_generator uuidGen(gen);

    // instanceId: 12 hex chars (no dashes), like QUuid::Id128.left(12)
    std::string fullUuid = uuids::to_string(uuidGen());
    std::string instanceId;
    for (char c : fullUuid)
        if (c != '-') instanceId += c;
    instanceId = instanceId.substr(0, 12);

    int64_t pid = getpid();

    logosctl::setEnvVar("LOGOS_INSTANCE_ID", instanceId.c_str());

    // Share the node with an OS group if the operator asked (--access-group).
    // Validate the group ONCE here, up front, so the socket policy and the
    // client-artifact policy agree: a typo'd group name is rejected in both
    // places (rather than exporting env vars for a group that
    // writeLocalClientArtifacts would then decline). When known-good, export
    // LOGOS_SOCKET_GROUP + LOGOS_SOCKET_MODE=0660 BEFORE logos_core_init so
    // every module subprocess (logos_host) and its children inherit it and
    // apply the policy to the local socket they bind (see logos-protocol's
    // applySocketPerms) — 0660 grants the write permission an AF_UNIX connect()
    // requires. `effectiveAccessGroup` (empty when unset or invalid) is what
    // gets handed to writeLocalClientArtifacts below.
    std::string effectiveAccessGroup = cfg.accessGroup;
#ifdef _WIN32
    // Refused outright rather than degraded to owner-only. Every mechanism the
    // feature is built from is POSIX: an OS group database (getgrnam_r), a gid
    // on the socket (chown), and a 0660 mode that an AF_UNIX connect() checks.
    // Windows has none of them — QLocalServer is a named pipe, whose access is
    // a security descriptor set at creation, and there is no group to name.
    // Accepting the flag and quietly not sharing would tell an operator their
    // node is group-restricted when it is in fact only user-restricted.
    if (!effectiveAccessGroup.empty()) {
        fprintf(stderr,
                "Error: --access-group is not supported on Windows. Sharing a "
                "node with an OS group needs POSIX group ownership and socket "
                "mode bits, which named pipes do not have.\n");
        return 1;
    }
#else
    if (!effectiveAccessGroup.empty()) {
        gid_t gid = 0;
        if (!resolveOsGroupGid(effectiveAccessGroup, gid)) {
            fprintf(stderr,
                    "Warning: --access-group '%s' is not a known group; the "
                    "daemon will not be shared.\n",
                    effectiveAccessGroup.c_str());
            effectiveAccessGroup.clear();
        } else {
            logosctl::setEnvVar("LOGOS_SOCKET_GROUP", effectiveAccessGroup.c_str());
            logosctl::setEnvVar("LOGOS_SOCKET_MODE", "0660");
        }
    }
#endif

    // Refuse to start if a live daemon already owns this config-dir — two would
    // clobber the shared state.json and re-issue the auto-token. Checked before
    // logos_core_init so it fails fast. A stale file from a crashed daemon (pid
    // gone) is not a live owner and is overwritten below.
    {
        const DaemonRuntimeState existing = DaemonRuntimeStateFile::read();
        if (existing.fileOk && existing.pid > 0
            && logosctl::processAlive(existing.pid)) {
            fprintf(stderr,
                    "Error: a logosctl daemon is already running in this config dir "
                    "(pid %lld, instance %s). Refusing to start a second one — use "
                    "--config-dir for a parallel instance.\n",
                    static_cast<long long>(existing.pid),
                    existing.instanceId.c_str());
            return 1;
        }
    }

    // Reap the socket files left behind by a previous node that died without
    // running its destructors. A graceful stop unlinks its own sockets (Qt's
    // QLocalServer destructor does it once QCoreApplication::exec() returns —
    // which is what the SIGTERM handler above enables), so this is purely for
    // the paths that cannot unwind: SIGKILL, a module crash, and the
    // PR_SET_PDEATHSIG kill that reaps orphaned logos_host children. Without
    // it those files accumulate in the temp dir forever, one per module per
    // boot.
    //
    // Deliberately placed AFTER the already-running check and BEFORE
    // logos_core_init: at this point no socket of ours exists yet, so the
    // reaper cannot race its own endpoints. A *co-resident* node's sockets are
    // live, and logos::isSocketDead fails closed — it unlinks only an S_ISSOCK
    // inode that we own and whose connect() is refused — so a second daemon
    // sharing the temp dir keeps its sockets, and a regular file that merely
    // shares the prefix (e.g. a logos_*.lgx build artefact) is never touched.
    //
    // QDir::tempPath() is the authoritative directory: QLocalServer resolves a
    // bare server name against it, so it is exactly where the sockets land, and
    // it honours $TMPDIR on both Linux and macOS.
    {
        const std::size_t reaped =
            logos::reapStaleSockets(QDir::tempPath().toStdString(), "logos_");
        if (reaped > 0 && verbose)
            fprintf(stderr, "Reaped %zu stale socket file(s) from %s\n",
                    reaped, QDir::tempPath().toStdString().c_str());
    }

    // 2. Initialize logos core
    logos_core_init(argc, argv);

    // 3. Add plugin directories — bundled first, then user-specified.
    //
    // ORDER IS PRECEDENCE, and later wins: the runtime scans these in order and
    // a module name seen twice keeps the last one (logos-package-manager's
    // enumerateManifests). So the bundle — what this binary happens to ship
    // with — goes first, and a directory the OPERATOR named on the command line
    // outranks it. That is the same rule the session modules directory below
    // already relies on by being added last.
    //
    // It used to be the other way round, which made `-m` the LOWEST precedence
    // of the three and meant an operator could not replace a bundled module at
    // all. `--container inproc` is where that stopped being theoretical: the
    // bundle ships capability_module as a Qt plugin, so a run that supplied the
    // Bare build of it in its own `-m` directory still loaded the Qt one into a
    // subprocess, and the daemon reported the in-process container as running
    // with its trust root outside it.
    const std::string bundledDir = paths::bundledModulesDir();
    if (!bundledDir.empty()) {
        logos_core_add_modules_dir(bundledDir.c_str());
        if (verbose)
            fprintf(stderr, "Added bundled modules directory: %s\n", bundledDir.c_str());
    }

    // Resolve to absolute paths: logos_core cannot load plugin metadata from
    // relative paths.
    for (const std::string& dir : modulesDirs) {
        std::error_code ec;
        std::string absDir = std::filesystem::absolute(dir, ec).string();
        const char* resolved = ec ? dir.c_str() : absDir.c_str();
        if (verbose)
            fprintf(stderr, "Added plugins directory: %s\n", resolved);
        logos_core_add_modules_dir(resolved);
    }

    // 3b. The session's own writable modules directory — where anything
    //     installed into this session lands. Without it on the search path,
    //     `install` would put a module on disk that the daemon could never
    //     see, so install-then-load could not work at all. Created eagerly so
    //     the package manager has somewhere to write on its very first
    //     install rather than failing on a missing directory.
    if (modern) {
        std::error_code ec;
        for (const std::string& dir : {Config::modulesDir(), Config::pluginsDir(),
                                       Config::keyringDir(), Config::cacheDir()}) {
            std::filesystem::create_directories(dir, ec);
        }
        logos_core_add_modules_dir(Config::modulesDir().c_str());
        // The bundled package modules live in their own directory so that
        // logoscore's module list is byte-identical to what it reports today.
        const std::string pkgDir = paths::bundledPackageModulesDir();
        if (!pkgDir.empty())
            logos_core_add_modules_dir(pkgDir.c_str());
        if (verbose)
            fprintf(stderr, "Added session modules directory: %s\n",
                    Config::modulesDir().c_str());
    }

    // 4. Set persistence base path for module instance data
    // Config::dataDir() already reflects dirs.data, and the loader folds the
    // older persistence_path spelling into it, so there is nothing to choose
    // between here.
    std::string persistenceBase = Config::dataDir();
    logos_core_set_persistence_base_path(persistenceBase.c_str());

    // 4a2. Install the container assertion before any module loads: it is
    //      evaluated per load, so it has to be in place before the first one.
    //      Empty => NULL, which the runtime reads as "auto".
    logos_core_set_container_policy(
        cfg.container.empty() ? nullptr : cfg.container.c_str());

    // 4b. Install the access policy before any module loads. Empty =>
    //     NULL (clear). Runtime side is currently a no-op.
    logos_core_set_access_policy(
        cfg.accessPolicy.empty() ? nullptr : cfg.accessPolicy.c_str());

    // 5. Materialize per-module transport sets BEFORE logos_core_start()
    //    so capability_module (loaded inside logos_core_start) gets the
    //    listeners the operator asked for, with ephemeral ports already
    //    allocated. Each module's transports come from the
    //    `--module-transport` CLI flags, fully decoupled — nothing about
    //    core_service's listeners leaks into capability_module's. The
    //    CLI defaulted both well-known modules to a LocalSocket-only
    //    entry if the operator didn't configure them.
    auto getModuleInfos = [&moduleTransports](const std::string& name) {
        auto it = moduleTransports.find(name);
        return it == moduleTransports.end()
                   ? std::vector<TransportInfo>{}
                   : it->second;
    };

    auto coreTransportsOpt = buildTransportSet(
        getModuleInfos("core_service"), "core_service");
    if (!coreTransportsOpt) {
        fprintf(stderr,
                "Daemon startup aborted: failed to build core_service transport set "
                "(see prior log lines for which listener failed).\n");
        return 1;
    }
    LogosTransportSet coreTransports = std::move(*coreTransportsOpt);

    auto capabilityTransportsOpt = buildTransportSet(
        getModuleInfos("capability_module"), "capability_module");
    if (!capabilityTransportsOpt) {
        fprintf(stderr,
                "Daemon startup aborted: failed to build capability_module transport set "
                "(see prior log lines for which listener failed).\n");
        return 1;
    }
    LogosTransportSet capabilityTransports = std::move(*capabilityTransportsOpt);

    // Register capability_module's transports with the runtime BEFORE
    // logos_core_start launches the child subprocess. The child reads
    // the JSON via --transport-set in its argv and uses the explicit-
    // transport LogosAPI constructor so its provider binds every
    // listener.
    {
        std::string capJson = logos::transportSetToJsonString(capabilityTransports);
        logos_core_set_module_transports("capability_module", capJson.c_str());
    }

    // 6. Start core (discover plugins, launch logos_host in remote mode).
    //    capability_module loads now, with the transport set we just
    //    registered.
    logos_core_start();

    // -v has to reach spdlog, not just Qt -- and it has to be set HERE.
    //
    // Module subprocesses do not share our stdio. The container gives each one
    // its own pipes, reads them line by line, and re-emits each line through
    // spdlog, picking the level from the line's prefix ("Debug:" -> debug).
    // spdlog's default is `info`, so a module's debug output was read, parsed,
    // classified -- and dropped at the last step. `-v` only ever gated OUR Qt
    // handler, so no flag made those lines appear.
    //
    // Ordering matters and cost a wrong fix: setting the level before
    // logos_core_start() is silently undone, because liblogos installs its own
    // `logos` logger during startup and that becomes the default. Set it after
    // and it sticks.
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);

    // 7. Register core_service as an in-process module via the C++ SDK.
    //    core_service can publish on multiple transports simultaneously:
    //    a local QLocalSocket (back-compat) + any TCP / TCP+SSL listeners
    //    specified via --transport.
    auto* coreServiceApi = new LogosAPI("core_service", coreTransports);
    auto* coreServiceImpl = new CoreServiceImpl();

    coreServiceImpl->init(coreServiceApi);
    auto* provider = coreServiceApi->getProvider();

    // Make operator-issued tokens (`logosctl token issue --name alice`) actually
    // authorize core_service calls. The built-in ModuleProxy scan only knows the
    // boot `auto` token and capability-minted tokens; this validator adds the
    // persisted token store, so a named token in daemon/tokens.json is accepted —
    // with its expiry and local_only flag enforced against the call's transport.
    // A fresh TokenStore per call reads tokens.json on demand, so revocation
    // (revoke-token) and expiry take effect immediately, without a restart.
    // Installed before registerObject so the proxy is validated from its first
    // published call.
    provider->setTokenValidator(
        [](const QString& token, const QString& transportProtocol) {
            TokenStore tokenStore;
            return tokenStore
                .lookupByToken(token.toStdString(), transportProtocol.toStdString())
                .has_value();
        });

    provider->registerObject("core_service", static_cast<LogosProviderObject*>(coreServiceImpl));

    // 8. Auto-issue a fresh `auto` token for this boot.
    //
    // Daemons store hashes at rest, so the raw value of any operator-
    // issued token (alice, bob, …) isn't recoverable on restart — those
    // tokens validate via TokenStore::lookupByToken on demand. The `auto`
    // token is special: the daemon (re-)generates it every boot,
    // overwrites both the hash entry in tokens.json and the raw files at
    // daemon/tokens/auto.json + client/auto.json, and registers the raw
    // version with the in-process TokenManager so the hot path stays
    // fast (no per-RPC hash + DB lookup). `local_only=true` keeps a
    // leaked client/auto.json from being usable over TCP from a remote
    // host. Operator-issued tokens take effect only on the next daemon
    // restart (or future SIGHUP-driven reload).
    TokenStore tokenStore;
    const auto autoTokenOutcome = tokenStore.issueToken("auto",
                                                        /*expiresAt=*/{},
                                                        /*localOnly=*/true,
                                                        /*replace=*/true);
    if (autoTokenOutcome.status != TokenStore::IssueStatus::Ok) {
        fprintf(stderr, "Failed to auto-issue local client token (status=%d)\n",
                static_cast<int>(autoTokenOutcome.status));
        // logos_core_start() already launched the module subprocesses; leaving
        // without cleanup strands them. Every other exit from this function
        // runs logos_core_cleanup() below.
        logos_core_cleanup();
        return 1;
    }
    const std::string autoTokenRaw = autoTokenOutcome.token;

    // saveINBOUND, not saveToken: "cli_client" is a CALLER of this daemon and
    // autoTokenRaw is the token it will PRESENT to us — not a credential we
    // present to it. The store is direction-split, so the outbound `saveToken`
    // files this under "when the daemon calls cli_client, send autoTokenRaw",
    // and the inbound check that authenticates the client's RPCs finds nothing.
    TokenManager::instance().saveInboundToken("cli_client", autoTokenRaw);

    // 8b. Bring up the bundled package modules and point them at this
    //     session's directories.
    //
    //     These are loaded unconditionally, like basecamp does after
    //     logos_core_start (app/main.cpp), because every package command is
    //     an RPC into them — a client that had to load them first would pay
    //     the cost on its first `package` command and race any concurrent
    //     client doing the same.
    //
    //     The directory configuration is the same four calls basecamp makes
    //     (PackageCoordinator::subscribeToPackageInstallationEvents): embedded
    //     is the read-only tree beside the binary, user is the session's
    //     writable tree. Without it the manager has no writable target and
    //     scans nothing, so `package ls` would report an empty session even
    //     after a successful install.
    //
    //     Failures here are logged, not fatal: a daemon that cannot manage
    //     packages is still a perfectly good daemon for loading and calling
    //     modules, and refusing to boot would turn a missing optional module
    //     into total unavailability.
    if (modern)
        bootstrapPackageModules(coreServiceApi, paths::bundledPackageModulesDir(),
                                cfg.signaturePolicy, verbose);

    // 9. Write the live-instance state file. Carries the resolved
    //    transport endpoints (post-bind, with real ports), instanceId/
    //    pid/startedAt for co-resident clients, and a snapshot of the
    //    operator-resolved config for diagnostics. Persistent state
    //    (tokens.json) and operator preferences (config.json, only
    //    written on --persist-config) live in their own files and
    //    aren't touched here.
    DaemonRuntimeState state;
    state.instanceId    = instanceId;
    state.pid           = pid;
    state.startedAt     = currentUtcIso8601();
    state.configSource  = configSource;
    // Start from the operator-merged config so downstream consumers
    // see every preference (ssl paths, insecureTcp), then
    // overwrite the per-module map with the resolved (post-bind)
    // transports — that's the only field where state.json diverges
    // from config.json on intent.
    state.resolved              = cfg;
    state.resolved.modules.clear();
    state.resolved.modules.emplace("core_service",      toAdvertised(coreTransports));
    state.resolved.modules.emplace("capability_module", toAdvertised(capabilityTransports));
    if (!DaemonRuntimeStateFile::write(state)) {
        fprintf(stderr, "Failed to write daemon state file: %s\n",
                DaemonRuntimeStateFile::filePath().c_str());
        // logos_core_start() already launched the module subprocesses; leaving
        // without cleanup strands them. Every other exit from this function
        // runs logos_core_cleanup() below.
        logos_core_cleanup();
        return 1;
    }

    // Persist operator preferences only if asked (legacy front-end only).
    // Done after state.json is on disk so a config that fails earlier (e.g. a
    // bind failure) doesn't pollute config.json.
    if (persistConfig) {
        if (DaemonConfigFile::write(cfg)) {
            fprintf(stdout, "Persisted config: %s\n",
                    DaemonConfigFile::filePath().c_str());
        } else {
            fprintf(stderr, "Warning: failed to persist config to %s\n",
                    DaemonConfigFile::filePath().c_str());
        }
    }

    // 10. Generate the local-client convenience artifacts (client/config.json
    //     + client/auto.json). The config.json write is gated inside
    //     writeLocalClientArtifacts on the file not already existing —
    //     remote clients are expected to write their own, and an
    //     operator-authored remote config must not be clobbered just
    //     because a daemon happened to start in the same config dir.
    //     The one exception: an existing config.json whose instance_id
    //     no longer matches this daemon is a stale copy of our own
    //     artifact (persisted config dir, replaced daemon) and is
    //     refreshed in place — see writeLocalClientArtifacts.
    //     The raw client/auto.json is always (re)written so a config.json
    //     pointing at "auto.json" stays consistent with the freshly-issued
    //     auto token.
    if (!DaemonRuntimeStateFile::writeLocalClientArtifacts(
            instanceId, autoTokenRaw, state.startedAt,
            toAdvertised(coreTransports),
            toAdvertised(capabilityTransports),
            effectiveAccessGroup)) {
        fprintf(stderr, "Warning: failed to write local client artifacts under %s\n",
                Config::clientDir().c_str());
    }

    fprintf(stdout, "Logosctl daemon started (pid %lld, instance %s)\n",
            static_cast<long long>(pid), instanceId.c_str());
    fprintf(stdout, "Daemon state: %s\n", DaemonRuntimeStateFile::filePath().c_str());
    fprintf(stdout, "Local client config: %s\n",
            Config::clientConfigPath().c_str());
    fflush(stdout);

    // 8. Set up signal handlers for clean shutdown. SIGINT / SIGTERM
    //    fire `QCoreApplication::quit()`, which makes `exec()` return
    //    and the explicit cleanup below runs. We also subscribe to
    //    `aboutToQuit` as a defense-in-depth: any future code path
    //    that calls `quit()` without going through the explicit
    //    cleanup (e.g. an exception caught by the event loop) still
    //    unlinks state.json so a co-resident client can detect
    //    "no live daemon".
    setupSignalHandlers();
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     []() { DaemonRuntimeStateFile::remove(); });

    // 9. Run Qt event loop (blocks)
    int result = QCoreApplication::exec();

    // 10. Cleanup
    fprintf(stdout, "Shutting down logosctl daemon...\n");
    fflush(stdout);

    logos_core_cleanup();
    LogSink::instance().stop();
    DaemonRuntimeStateFile::remove();

    delete coreServiceImpl;
    delete coreServiceApi;

    fprintf(stdout, "Logosctl daemon stopped.\n");
    fflush(stdout);

    return result;
}
