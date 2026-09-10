#include <CLI/CLI.hpp>
#include <QCoreApplication>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "config.h"
#include "paths.h"
#include "platform_compat.h"
#include "daemon/daemon.h"
#include "daemon/daemon_state.h"
#include "daemon/access_policy_arg.h"
#include "client/client_state.h"
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"
#include "logos_core.h"
#include "version_info.h"

static bool g_verbose = false;

static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    QByteArray localMsg = msg.toLocal8Bit();
    const char *file = context.file ? context.file : "";
    const char *function = context.function ? context.function : "";

    switch (type) {
    case QtDebugMsg:
        if (!g_verbose) return;
        fprintf(stderr, "Debug: %s\n", localMsg.constData());
        break;
    case QtInfoMsg:
        if (!g_verbose) return;
        fprintf(stderr, "Info: %s\n", localMsg.constData());
        break;
    case QtWarningMsg:
        if (!g_verbose) return;
        fprintf(stderr, "Warning: %s\n", localMsg.constData());
        break;
    case QtCriticalMsg:
        fprintf(stderr, "Critical: %s (%s:%d, %s)\n", localMsg.constData(), file, context.line, function);
        break;
    case QtFatalMsg:
        fprintf(stderr, "Fatal: %s (%s:%d, %s)\n", localMsg.constData(), file, context.line, function);
        fflush(stderr);
        abort();
    }
    fflush(stderr);
}

// Pre-scan argv for `--config-dir` so we can apply the override (and
// resolve the corresponding `<configDir>/config.json` path) *before*
// CLI11 parses anything else. Returns the override path or empty if
// not present. Recognises both `--config-dir X` (two tokens) and
// `--config-dir=X` (single token); mirrors what CLI11 would parse.
static std::string preScanConfigDir(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config-dir" && i + 1 < argc) return argv[i + 1];
        const std::string prefix = "--config-dir=";
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return {};
}

// Resolve the --access-policy argument (see daemon/access_policy_arg.h for the
// three accepted spellings) and print the reason on stderr if it can't be.
static std::optional<std::string> resolveAccessPolicy(const std::string& arg)
{
    std::string error;
    auto resolved = logoscore::resolveAccessPolicyArg(arg, &error);
    if (!resolved)
        std::cerr << "Error: " << error << std::endl;
    return resolved;
}

int main(int argc, char *argv[])
{
    // This binary is logoscore: the surface that exists today,
    // ~/.logoscore, JSON config. logosctl ships alongside it and
    // shares no state; this one changes only when it must.
    Config::setFlavor(Config::Flavor::Legacy);

    // Pre-scan argv for `--config-dir` so the override applies before
    // any Config::* call. The CLI11 parse below picks the same flag up
    // again and re-applies it, but we need it earlier than that —
    // anything that touches Config::configDir during option parsing
    // (e.g. logging) would otherwise key off the wrong directory.
    {
        std::string preDir = preScanConfigDir(argc, argv);
        if (!preDir.empty()) {
            std::error_code ec;
            const std::filesystem::path absCfgPath =
                std::filesystem::absolute(preDir, ec);
        if (!ec)
                Config::setConfigDir(absCfgPath.string());
    }
    }

    // The config tree splits by lifetime under <configDir>/:
    //   daemon/config.json   — operator preferences (writes only on --persist-config)
    //   daemon/state.json    — live runtime state (created at boot, removed at shutdown)
    //   daemon/tokens.json   — hashed-at-rest accepted tokens (survives restarts)
    //   client/config.json   — client dial spec (writes only on --persist-config)
    // CLI flags merge over disk via per-flag Option::count() detection.

    // ── CLI11 setup ──────────────────────────────────────────────────────────
    CLI::App app{"logoscore - Logos Core runtime CLI"};
    app.set_version_flag("--version", logosctl_version::versionString("logoscore"));
    app.set_help_flag("-h,--help", "Show this help");

    // Global flags
    app.add_flag("-v,--verbose", g_verbose, "Show debug logs");

    // Client global flags (defined at app level so they work before subcommands;
    // also extracted from subcommand remaining() for placement after subcommands)
    bool jsonMode = false;
    app.add_flag("-j,--json", jsonMode, "Force JSON output");

    bool humanMode = false;
    app.add_flag("--no-json,--human", humanMode,
                 "Force human-readable output even when piped");

    bool quiet = false;
    app.add_flag("-q,--quiet", quiet, "Suppress non-essential output");

    // Daemon flag (-D as shorthand for daemon subcommand)
    bool daemonFlag = false;
    app.add_flag("-D", daemonFlag, "Start the daemon process");

    // Shared option: modules directory (used by the daemon)
    std::vector<std::string> modulesDirs;
    auto* modulesDirOpt = app.add_option("-m,--modules-dir", modulesDirs,
        "Module search directory (repeatable)");

    std::string persistencePath;
    auto* persistencePathOpt = app.add_option("--persistence-path", persistencePath,
        "Base directory for module instance persistence (default: ~/.logoscore/data)");

    // --container: which container this daemon's modules must run in.
    //
    // An ASSERTION, not a switch. Which container runs a module is decided by
    // its ARTIFACT — a Bare module image runs in-process in the Native
    // container, a Qt plugin runs in a subprocess host, a web variant runs in a
    // webview in the Web container — and no flag can turn one into another.
    // `--container inproc` therefore means "every module here must be a Bare
    // module", and a Qt plugin under it is refused rather than quietly
    // subprocessed. Without that, the flag would mean "in-process if you happen
    // to have built it that way", which is not something a CI job can assert or
    // an operator can rely on.
    //
    // `--container web` additionally needs this process to HAVE a webview: a
    // page is not something a headless binary can run by wanting to. The
    // runtime reports the missing bridge by name when a web module is loaded
    // without one (LogosCore::setWebModuleViewFactory).
    std::string containerArg;
    auto* containerOpt = app.add_option("--container", containerArg,
        "Require modules to run in a specific container: auto (default) | "
        "inproc (every module must be a Bare module) | subprocess (every module "
        "must be a Qt plugin) | web (every module must be a web variant)");
    containerOpt->check(CLI::IsMember({"auto", "inproc", "subprocess", "web"}));

    // --access-policy: inter-module access policy (the literal `enforce`, a
    // file path, or inline JSON). Daemon-only; forwarded to the runtime before
    // modules load. Absent => no policy => enforcement off, as before.
    std::string accessPolicyArg;
    auto* accessPolicyOpt = app.add_option("--access-policy", accessPolicyArg,
        "Inter-module access policy (default: none, no enforcement). "
        "`enforce` turns on deny-by-default: a module may only call the "
        "modules it declares as dependencies, and any other call is refused. "
        "Also accepts a path to a JSON file, or inline JSON "
        "(mode + per-target caller allowlists)");

    // --access-group: share the daemon with an OS group. Sockets become
    // group-connectable (0660, chgrp'd) and the client artifacts group-readable,
    // so a second OS user in the group can drive the daemon (docker.sock model).
    std::string accessGroupArg;
    auto* accessGroupOpt = app.add_option("--access-group", accessGroupArg,
        "OS group to share the daemon with: makes the local sockets and client "
        "config/token group-accessible so a member can run logoscore commands");

    // --persist-config: write the merged (defaults < config.json < CLI)
    // result to disk. Without it, CLI flags affect the running process
    // only; with it, the next no-flag launch reproduces the same
    // behavior. Applies symmetrically to daemon (daemon/config.json) and
    // client (client/config.json) modes.
    bool persistConfig = false;
    app.add_flag("--persist-config", persistConfig,
        "Write the merged config to config.json so the next launch reproduces these flags");

    // Override the config dir (daemon/{config,state,tokens}.json,
    // client/config.json, data/) so parallel logoscore instances can
    // run side-by-side. Client commands must be invoked with the
    // same --config-dir as the daemon they target.
    std::string configDirStr;
    app.add_option("--config-dir", configDirStr,
        "Override config directory (default: ~/.logoscore; also LOGOSCORE_CONFIG_DIR)");

    // ── Transport flags (daemon side) ────────────────────────────────────────
    // Per-module transport configuration. Each `--module-transport`
    // adds one listener to the named module. Format:
    //
    //     NAME=PROTOCOL[,k=v[,k=v...]]
    //
    // PROTOCOL is `local`, `tcp`, or `tcp_ssl`. Recognized k=v pairs:
    //
    //     host         (tcp / tcp_ssl)
    //     port         (tcp / tcp_ssl; 0 = auto-allocate ephemeral)
    //     codec        (tcp / tcp_ssl; "json" default | "cbor")
    //     ca           (tcp_ssl; CA cert path for client verification)
    //     cert,key     (tcp_ssl; server cert + key paths)
    //     verify_peer  (tcp_ssl; "true"|"false", default true)
    //
    // Repeatable. Each module gets its own list — there is no
    // "core_service is the source, capability_module inherits" magic
    // any more. Operators are expected to configure each module
    // explicitly, matching the on-disk shape of daemon/state.json's
    // resolved.modules block.
    //
    // Default when omitted: each well-known module
    // (`core_service`, `capability_module`) gets a single `local`
    // listener.
    //
    // Examples:
    //     logoscore -D \
    //         --module-transport core_service=local \
    //         --module-transport core_service=tcp,host=0.0.0.0,port=6000,codec=json \
    //         --module-transport capability_module=local \
    //         --module-transport capability_module=tcp,host=0.0.0.0,port=6001,codec=json
    std::vector<std::string> moduleTransportFlags;
    auto* moduleTransportOpt = app.add_option("--module-transport", moduleTransportFlags,
        "Configure a module's transport: NAME=PROTOCOL[,k=v...] (repeatable)");

    // Plaintext-TCP safety net: refuse to bind plaintext `tcp` on a
    // non-loopback host unless this flag is set. Without it, a daemon
    // configured with `--module-transport core_service=tcp,host=0.0.0.0`
    // would put tokens on the wire in cleartext on every RPC. The
    // escape hatch exists for trusted-network test setups; production
    // use should pass tcp_ssl or wrap with a TLS terminator.
    bool insecureTcp = false;
    auto* insecureTcpOpt = app.add_flag("--insecure-tcp", insecureTcp,
        "Allow plaintext tcp on non-loopback hosts (tokens travel cleartext)");

    // ── Transport flags (client side) ────────────────────────────────────────
    //
    // Each flag has a matching `LOGOSCORE_CLIENT_*` env-var fallback,
    // so callers that drive logoscore as a subprocess (e.g. the Python
    // wrapper) can configure the dial spec without manipulating CLI
    // strings. CLI flag still wins over the env var when both are set
    // — same precedence as anywhere else in the program.
    std::string clientTransport;  // empty = prefer local
    auto* clientTransportOpt = app.add_option("--client-transport", clientTransport,
        "Pick one of the daemon's advertised transports: local | tcp | tcp_ssl");
    clientTransportOpt->envname("LOGOSCORE_CLIENT_TRANSPORT");
    std::string clientTcpHost;
    auto* clientTcpHostOpt = app.add_option("--client-tcp-host", clientTcpHost,
        "Override the daemon's advertised host (e.g. 'localhost' when daemon bound 0.0.0.0 in docker)");
    clientTcpHostOpt->envname("LOGOSCORE_CLIENT_TCP_HOST");
    uint16_t clientTcpPort = 0;
    auto* clientTcpPortOpt = app.add_option("--client-tcp-port", clientTcpPort,
        "Override the daemon's advertised port (useful when port-forwarding or NAT changes the reachable port)");
    clientTcpPortOpt->envname("LOGOSCORE_CLIENT_TCP_PORT");
    bool clientNoVerifyPeer = false;
    auto* clientNoVerifyPeerOpt = app.add_flag("--no-verify-peer", clientNoVerifyPeer,
        "Disable TLS peer verification (dev only)");
    clientNoVerifyPeerOpt->envname("LOGOSCORE_CLIENT_NO_VERIFY_PEER");
    std::string clientCodec;  // empty = accept whatever the daemon advertised
    auto* clientCodecOpt = app.add_option("--client-codec", clientCodec,
        "Require a specific wire codec (json | cbor); if the daemon advertised "
        "a different codec for the picked transport, connect fails.");
    clientCodecOpt->envname("LOGOSCORE_CLIENT_CODEC");
    std::string clientTokenFile;
    auto* clientTokenFileOpt = app.add_option("--token-file", clientTokenFile,
        "Filename inside client/ to use for authentication (must already exist; "
        "no copy semantics)");
    clientTokenFileOpt->envname("LOGOSCORE_CLIENT_TOKEN_FILE");
    std::string clientSslCa;
    auto* clientSslCaOpt = app.add_option("--ssl-ca", clientSslCa,
        "CA cert path used to verify the daemon's TLS chain (tcp_ssl only)");
    clientSslCaOpt->envname("LOGOSCORE_CLIENT_SSL_CA");

    // ── Client subcommands ───────────────────────────────────────────────────
    // All client subcommands use allow_extras() so their positional args and
    // command-specific flags (--loaded, --event) are captured in remaining().
    // Global flags (--json, --quiet) mixed in after the subcommand are also
    // captured and extracted before dispatching to the command object.

    auto* daemonSub  = app.add_subcommand("daemon", "Start the daemon process");
    daemonSub->fallthrough();  // -m, -v after "daemon" fall through to parent

    auto* statusSub        = app.add_subcommand("status", "Show daemon and module health");
    auto* loadModuleSub    = app.add_subcommand("load-module", "Load a module into the daemon");
    auto* unloadModuleSub  = app.add_subcommand("unload-module", "Unload a module from the daemon");
    auto* reloadModuleSub  = app.add_subcommand("reload-module", "Reload (unload + load) a module");
    auto* listModulesSub   = app.add_subcommand("list-modules", "List available or loaded modules");
    auto* moduleInfoSub    = app.add_subcommand("module-info", "Show detailed module information");
    auto* infoSub          = app.add_subcommand("info", "Alias for module-info");
    auto* callSub          = app.add_subcommand("call", "Call a method on a loaded module");
    auto* moduleSub        = app.add_subcommand("module", "Call a method (verbose syntax)");
    auto* watchSub         = app.add_subcommand("watch", "Watch events from a module");
    auto* statsSub         = app.add_subcommand("stats", "Show module resource usage");
    auto* stopSub          = app.add_subcommand("stop", "Stop the daemon");

    // Token-management subcommands. These operate directly on the config
    // dir (no daemon connection required), so they can be used offline to
    // prepare client credentials before the daemon even starts.
    auto* issueTokenSub    = app.add_subcommand("issue-token",
        "Issue a new client token under --name NAME");
    auto* revokeTokenSub   = app.add_subcommand("revoke-token",
        "Revoke a previously-issued client token");
    auto* listTokensSub    = app.add_subcommand("list-tokens",
        "List the names of issued client tokens");

    // Allow extras on all client subcommands so their positional args and
    // command-specific flags pass through to the Command objects unchanged
    for (auto* sub : {statusSub, loadModuleSub, unloadModuleSub, reloadModuleSub,
                      listModulesSub, moduleInfoSub, infoSub, callSub, moduleSub,
                      watchSub, statsSub, stopSub,
                      issueTokenSub, revokeTokenSub, listTokensSub}) {
        sub->allow_extras();
    }

    app.require_subcommand(0, 1);  // 0 or 1 subcommand

    // ── Parse ────────────────────────────────────────────────────────────────
    CLI11_PARSE(app, argc, argv);

    qInstallMessageHandler(messageHandler);

    // Apply --config-dir (if passed) before any Config::* call so the daemon,
    // client, connection_file, and any forked logos_host all see the same
    // config dir. Also mirror into the env var so child processes inherit it.
    if (!configDirStr.empty()) {
        std::error_code ec;
        const std::filesystem::path absCfgPath =
            std::filesystem::absolute(configDirStr, ec);
        if (ec) {
            std::cerr << "Error: failed to resolve --config-dir '" << configDirStr
                      << "': " << ec.message() << std::endl;
            return 1;
        }
        std::filesystem::create_directories(absCfgPath, ec);
        if (ec) {
            std::cerr << "Error: failed to create --config-dir '" << absCfgPath.string()
                      << "': " << ec.message() << std::endl;
            return 1;
        }
        Config::setConfigDir(absCfgPath.string());
        logosctl::setEnvVar("LOGOSCORE_CONFIG_DIR", absCfgPath.string().c_str());
    }

    // ── Daemon mode ──────────────────────────────────────────────────────────
    if (daemonFlag || daemonSub->parsed()) {
        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion(QString::fromStdString(logosctl_version::version()));

        // Plaintext-TCP guard: a `tcp` listener on a non-loopback host
        // sends tokens in cleartext. Refuse to start unless the
        // operator explicitly opted in.
        auto isLoopback = [](const std::string& h) {
            return h == "127.0.0.1" || h == "::1" || h == "localhost";
        };
        auto validateCodec = [](const std::string& c) {
            return c == "json" || c == "cbor";
        };

        // Parse --module-transport NAME=PROTOCOL[,k=v...] flags into
        // a per-module map. Each flag adds one listener to the named
        // module. There is deliberately no implicit core_service /
        // capability_module relationship — each module's transport
        // list is built solely from its own flags, then defaulted
        // to a single LocalSocket entry below for the well-known
        // modules if the operator didn't configure them.
        std::map<std::string, std::vector<TransportInfo>> moduleTransportsMap;
        for (const auto& spec : moduleTransportFlags) {
            const auto eq = spec.find('=');
            if (eq == std::string::npos || eq == 0 || eq == spec.size() - 1) {
                std::cerr << "Error: --module-transport expects "
                          << "'NAME=PROTOCOL[,k=v...]', got: '" << spec
                          << "'" << std::endl;
                return 1;
            }
            const std::string moduleName = spec.substr(0, eq);
            const std::string body = spec.substr(eq + 1);

            // Split body on commas. The first part is the protocol;
            // the rest are key=value pairs. Splitting comma-separated
            // is safe — neither host names nor port numbers nor codec
            // names contain commas; cert paths on POSIX don't either.
            std::vector<std::string> parts;
            for (size_t i = 0; i < body.size(); ) {
                auto comma = body.find(',', i);
                parts.push_back(body.substr(i, comma == std::string::npos
                                                  ? std::string::npos
                                                  : comma - i));
                if (comma == std::string::npos) break;
                i = comma + 1;
            }
            if (parts.empty() || parts[0].empty()) {
                std::cerr << "Error: --module-transport '" << spec
                          << "' missing protocol" << std::endl;
                return 1;
            }

            TransportInfo t;
            t.protocol = parts[0];

            // Defaults for tcp / tcp_ssl when the operator omits
            // them. host="127.0.0.1" matches the daemon-side bind
            // convention used by older flags and keeps a bare
            // `--module-transport core_service=tcp` working out of
            // the box on a single host.
            if (t.protocol == "tcp" || t.protocol == "tcp_ssl") {
                t.host = "127.0.0.1";
                t.codec = "json";
                t.verifyPeer = true;
            }

            for (size_t i = 1; i < parts.size(); ++i) {
                const auto& kv = parts[i];
                const auto kvSep = kv.find('=');
                if (kvSep == std::string::npos) {
                    std::cerr << "Error: --module-transport '" << spec
                              << "' has malformed kv pair '" << kv
                              << "' (expected k=v)" << std::endl;
                    return 1;
                }
                const std::string k = kv.substr(0, kvSep);
                const std::string v = kv.substr(kvSep + 1);
                if      (k == "host")  t.host = v;
                else if (k == "codec") t.codec = v;
                else if (k == "ca")    t.caFile = v;
                else if (k == "cert")  t.certFile = v;
                else if (k == "key")   t.keyFile = v;
                else if (k == "verify_peer") {
                    // Strict allowlist: a typo like `verify_peer=treu`
                    // would otherwise silently match the false branch
                    // and disable TLS peer verification, weakening
                    // security without any visible signal.
                    if      (v == "true"  || v == "1") t.verifyPeer = true;
                    else if (v == "false" || v == "0") t.verifyPeer = false;
                    else {
                        std::cerr << "Error: --module-transport verify_peer '"
                                  << v << "' must be one of true|false|1|0"
                                  << std::endl;
                        return 1;
                    }
                }
                else if (k == "port") {
                    // Require the WHOLE value to parse — std::stoi would accept
                    // "6000x" as 6000 / "0x1F90" as 0, silently binding the wrong port.
                    int parsedPort = 0;
                    size_t consumed = 0;
                    try { parsedPort = std::stoi(v, &consumed); }
                    catch (...) { consumed = 0; }
                    if (v.empty() || consumed != v.size()) {
                        std::cerr << "Error: --module-transport port '" << v
                                  << "' is not a valid integer" << std::endl;
                        return 1;
                    }
                    if (parsedPort < 0 || parsedPort > 0xFFFF) {
                        std::cerr << "Error: --module-transport port '" << v
                                  << "' must be in [0, 65535]" << std::endl;
                        return 1;
                    }
                    t.port = static_cast<uint16_t>(parsedPort);
                } else {
                    std::cerr << "Error: --module-transport unknown key '"
                              << k << "' in '" << spec << "'" << std::endl;
                    return 1;
                }
            }

            // Per-protocol validation.
            if (t.protocol == "local") {
                // host/port/codec/cert ignored for local.
            } else if (t.protocol == "tcp") {
                if (!validateCodec(t.codec)) {
                    std::cerr << "Error: --module-transport tcp codec '"
                              << t.codec << "' must be 'json' or 'cbor'"
                              << std::endl;
                    return 1;
                }
                // Plaintext-TCP guard runs post-merge below so disk-fed
                // listeners are also covered.
            } else if (t.protocol == "tcp_ssl") {
                if (!validateCodec(t.codec)) {
                    std::cerr << "Error: --module-transport tcp_ssl codec '"
                              << t.codec << "' must be 'json' or 'cbor'"
                              << std::endl;
                    return 1;
                }
                if (t.certFile.empty() || t.keyFile.empty()) {
                    std::cerr << "Error: --module-transport tcp_ssl for '"
                              << moduleName << "' requires cert= and key="
                              << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: --module-transport unknown protocol '"
                          << t.protocol << "' (expected local | tcp | tcp_ssl)"
                          << std::endl;
                return 1;
            }

            moduleTransportsMap[moduleName].push_back(std::move(t));
        }

        // Per-flag merge: load disk config (if any), then layer CLI
        // overrides on top — but only for flags the operator
        // explicitly passed. CLI11's Option::count() is the only
        // accurate signal: a default-valued local var is
        // indistinguishable from an explicit `--persistence-path ""`
        // without it. Anything not touched by either CLI or disk
        // falls through to defaults.
        DaemonConfig mergedCfg;
        std::string  configSource = "defaults";

        if (auto disk = DaemonConfigFile::read()) {
            mergedCfg = *disk;
            configSource = "config.json";
        }

        const bool anyCliFlag = (modulesDirOpt->count()      > 0)
                             || (persistencePathOpt->count() > 0)
                             || (moduleTransportOpt->count() > 0)
                             || (insecureTcpOpt->count()     > 0)
                             || (accessPolicyOpt->count()    > 0)
                             || (containerOpt->count()       > 0)
                             || (accessGroupOpt->count()     > 0);
        if (anyCliFlag) configSource = "cli";

        if (modulesDirOpt->count() > 0)      mergedCfg.modulesDirs     = modulesDirs;
        // --persistence-path has to reach the field the daemon actually reads
        // — dirs.data — and not only its older spelling. The alias that folds
        // persistence_path into dirs.data lives inside daemonConfigFromJson,
        // so it runs while READING a config file and never after this merge:
        // assigning persistencePath alone left the flag with no consumer at
        // all. Writing both gives this block the precedence it documents
        // (explicit command line > config file > default) and keeps the two
        // spellings agreeing in whatever --persist-config writes back.
        //
        // Absolutised first, because the two sources spell relative paths
        // differently: a `dirs:` entry in config.json is relative to the config
        // dir (Config::setSessionDirOverride resolves it there), while a path
        // typed on the command line is relative to the shell's cwd — what this
        // flag has always meant, and what --modules-dir still means. Pinning it
        // here keeps `--persistence-path ./data` pointing at ./data instead of
        // quietly reparenting it under ~/.logoscore. An explicit empty value
        // stays empty: that is how both fields already encode "no override,
        // use the default", and absolute("") would turn it into the cwd.
        //
        // A leading `~/` is left ALONE so that resolveOverride expands it
        // (config.cpp:107). Absolutising it first would produce `$PWD/~/x` — a
        // real directory literally named `~` — and would make the flag and a
        // config.json `dirs.data` disagree about the same string. The shell
        // expands an unquoted `~/x` before we ever see it, so this only governs
        // a quoted or script-built value, which is exactly the case where the
        // user cannot have meant a directory called `~`.
        if (persistencePathOpt->count() > 0) {
            std::string resolved = persistencePath;
            const bool tilde = !resolved.empty() && resolved[0] == '~'
                            && (resolved.size() == 1 || resolved[1] == '/');
            if (!resolved.empty() && !tilde) {
                std::error_code ec;
                std::filesystem::path abs =
                    std::filesystem::absolute(resolved, ec);
                if (!ec) resolved = abs.lexically_normal().string();
            }
            mergedCfg.persistencePath = resolved;
            mergedCfg.dirs.data       = resolved;
        }
        if (insecureTcpOpt->count() > 0)     mergedCfg.insecureTcp     = insecureTcp;
        if (containerOpt->count() > 0)       mergedCfg.container       = containerArg;
        if (accessGroupOpt->count() > 0)     mergedCfg.accessGroup     = accessGroupArg;
        // Resolve --access-policy (file-or-inline); abort on bad input.
        if (accessPolicyOpt->count() > 0) {
            auto resolved = resolveAccessPolicy(accessPolicyArg);
            if (!resolved) return 1;
            mergedCfg.accessPolicy = std::move(*resolved);
        }
        // --module-transport replaces the disk's modules wholesale
        // when the operator passes any. There's no per-module merge:
        // mixing operator intent with stale disk entries leads to
        // surprising behavior (a flag that disabled a listener on
        // disk would silently re-enable it). Either operator-specified
        // or disk-specified — never a hybrid.
        if (moduleTransportOpt->count() > 0) mergedCfg.modules = moduleTransportsMap;

        // Make sure the well-known modules at least *have* an entry,
        // so a bare `logoscore -D` (no transport flags) still boots
        // with listeners. The local-prepend below populates them.
        for (const std::string& wellKnown : {"core_service", "capability_module"}) {
            (void)mergedCfg.modules[wellKnown];  // default-construct empty
        }

        // Always make every configured module carry a LocalSocket
        // listener. Two reasons:
        //
        //  (1) Default modules (none operator-configured) need *some*
        //      listener — local is the cheapest, always-works choice.
        //
        //  (2) Even when the operator explicitly opts into TCP / TCP+SSL
        //      for a given module (e.g.
        //      `--module-transport core_service=tcp,...`,
        //      `--module-transport my_module=tcp,...`), a lot of
        //      intra-daemon code paths (capability_module's
        //      requestModule → core_service handshake; the daemon's
        //      own auto-`requestModule` flow inside LogosAPIClient;
        //      cross-module outbound `getClient(name)` calls) default
        //      to LocalSocket and have no plumbing to discover the
        //      operator's chosen TCP endpoint. Forcing a local listener
        //      alongside whatever else the operator named keeps those
        //      paths working without fan-out — the operator's TCP
        //      listener is the *additional* surface for outside clients.
        //
        // Order matters: we PREPEND local so it's the first entry in
        // each module's preference list, which means consumers that
        // pick "first transport" land on local. Operator-supplied
        // entries follow in the order they were typed.
        //
        // Applies to every module in the merged config, well-known or
        // user-configured — same logic, no special-casing.
        //
        // Normalization rule: at most one `local` entry per module,
        // always at index 0. If the operator typed `local` later in
        // the order (e.g. `--module-transport NAME=tcp,...
        // --module-transport NAME=local`) we MOVE that entry to the
        // front rather than leave it at index 1 and prepend a fresh
        // one — otherwise consumers that pick "first transport" would
        // still land on TCP, and we'd have two local entries to dedupe.
        for (auto& [moduleName, transports] : mergedCfg.modules) {
            (void)moduleName;
            auto localIt = std::find_if(transports.begin(), transports.end(),
                [](const TransportInfo& t) { return t.protocol == "local"; });
            TransportInfo localEntry;
            if (localIt != transports.end()) {
                localEntry = std::move(*localIt);
                transports.erase(localIt);
            } else {
                localEntry.protocol = "local";
            }
            transports.insert(transports.begin(), std::move(localEntry));
        }

        // Plaintext-TCP guard, post-merge: refuse to bind plaintext
        // tcp on a non-loopback host unless `insecure_tcp` is enabled
        // (whether by --insecure-tcp on the CLI or by config.json).
        // Iterating the merged map means a disk-supplied plaintext
        // listener gets the same scrutiny as a CLI-supplied one — the
        // operator can't bypass the guard by stashing the combo in
        // config.json.
        for (const auto& [moduleName, transports] : mergedCfg.modules) {
            for (const auto& t : transports) {
                if (t.protocol != "tcp") continue;
                if (isLoopback(t.host)) continue;
                if (mergedCfg.insecureTcp) continue;
                std::cerr << "Error: module '" << moduleName
                          << "' binds plaintext tcp on non-loopback host '"
                          << t.host << "'. Use protocol=tcp_ssl, or pass "
                          << "--insecure-tcp if you really mean it."
                          << std::endl;
                return 1;
            }
        }

        return Daemon::start(argc, argv, mergedCfg, configSource, persistConfig, g_verbose);
    }

    // ── Client-side per-flag merge ───────────────────────────────────────────
    // Same precedence as the daemon side: defaults < client/config.json
    // < CLI args. CLI11's Option::count() drives per-flag override
    // detection. If the operator passed any client-config flag, the
    // merged result takes effect for this run; if `--persist-config`
    // is also passed, the merged result is written back to
    // client/config.json so subsequent no-flag launches reproduce it.
    {
        const bool anyClientCfgFlag = (clientTransportOpt->count()    > 0)
                                   || (clientTcpHostOpt->count()      > 0)
                                   || (clientTcpPortOpt->count()      > 0)
                                   || (clientNoVerifyPeerOpt->count() > 0)
                                   || (clientCodecOpt->count()        > 0)
                                   || (clientTokenFileOpt->count()    > 0)
                                   || (clientSslCaOpt->count()        > 0);

        if (anyClientCfgFlag || persistConfig) {
            // Validate --client-codec up front; otherwise a typo is stored
            // verbatim and silently coerced to JSON at dial time.
            if (clientCodecOpt->count() > 0
             && clientCodec != "json" && clientCodec != "cbor") {
                std::cerr << "Error: --client-codec '" << clientCodec
                          << "' must be 'json' or 'cbor'" << std::endl;
                return 1;
            }

            ClientState merged = ClientStateFile::read();  // disk (or empty)

            // The transport-shape overrides apply to BOTH dialed
            // modules (core_service and capability_module) since
            // both go through the same daemon endpoint. An operator
            // who needs per-module divergence has to hand-edit
            // client/config.json — keeping the CLI surface small.
            auto applyToModule = [&](const std::string& moduleName) {
                ClientModuleTransport& t = merged.daemon[moduleName];
                if (clientTransportOpt->count() > 0) t.protocol = clientTransport;
                if (clientTcpHostOpt->count()   > 0) t.host     = clientTcpHost;
                if (clientTcpPortOpt->count()   > 0) t.port     = clientTcpPort;
                if (clientCodecOpt->count()     > 0) t.codec    = clientCodec;
                if (clientNoVerifyPeerOpt->count() > 0) t.verifyPeer = !clientNoVerifyPeer;
                if (clientSslCaOpt->count()     > 0) t.caFile   = clientSslCa;
                // Default protocol when this module is being
                // freshly added by CLI flags (i.e. nothing on disk
                // and the operator didn't pick a transport).
                if (t.protocol.empty()) t.protocol = "local";
            };
            applyToModule("core_service");
            applyToModule("capability_module");

            if (clientTokenFileOpt->count() > 0) {
                merged.tokenFile = clientTokenFile;
                // Refuse to start if the named raw-token file isn't
                // already under client/. No copy semantics — the
                // operator is expected to scp the daemon-side
                // tokens/<name>.json into place themselves.
                std::error_code ec;
                if (!std::filesystem::exists(
                        Config::clientTokenPath(clientTokenFile), ec)) {
                    std::cerr << "Error: --token-file '" << clientTokenFile
                              << "' does not exist at "
                              << Config::clientDir() << "/" << clientTokenFile
                              << ". Copy it from the daemon's daemon/tokens/ dir first."
                              << std::endl;
                    return 1;
                }
                // Existence isn't enough — validate the content now so a file
                // with no usable token errors here, not later at connect time.
                if (ClientStateFile::readTokenFile(clientTokenFile).empty()) {
                    std::cerr << "Error: --token-file '" << clientTokenFile
                              << "' at " << Config::clientTokenPath(clientTokenFile)
                              << " has no usable 'token' field (missing key, "
                                 "empty, or unparseable JSON)."
                              << std::endl;
                    return 1;
                }
            }

            // Stamp the schema version in case the merge built it
            // up from defaults — the on-disk path needs it for the
            // version check in ClientStateFile::read.
            merged.schemaVersion = kClientStateSchemaVersion;
            // `fileOk` is the "this is usable for dialing" bit;
            // RpcClient::connect checks it. The merge guarantees
            // every run has at least one daemon entry, so fileOk
            // is true iff a token_file is also set.
            merged.fileOk = !merged.daemon.empty() && !merged.tokenFile.empty();

            // Inject merged state into ClientStateFile so the
            // override applies for this run regardless of disk state.
            ClientStateFile::setOverride(merged);

            if (persistConfig) {
                if (ClientStateFile::write(merged)) {
                    fprintf(stdout, "Persisted client config: %s\n",
                            ClientStateFile::filePath().c_str());
                } else {
                    fprintf(stderr, "Warning: failed to persist client config to %s\n",
                            ClientStateFile::filePath().c_str());
                }
            }
        }
    }

    // -m/-l/--persistence-path configure the daemon (-D) only. Inline (-c) mode
    // has been removed, so these flags are meaningless for a client subcommand
    // or a bare invocation — reject them with daemon/client guidance rather than
    // silently ignoring them.
    auto rejectDaemonOnlyFlags = [&]() -> bool {
        if (modulesDirOpt->count() == 0 && persistencePathOpt->count() == 0)
            return false;
        std::cerr <<
            "Error: -m/--modules-dir and --persistence-path apply only to the "
            "daemon (-D); inline (-c) mode has been removed. "
            "Use daemon + client commands:\n"
            "  logoscore -D -m <dir>                     # start a daemon (clean)\n"
            "  logoscore load-module <module>            # load a module\n"
            "  logoscore call <module> <method> [args]   # call a method\n";
        return true;
    };

    // ── Client mode ──────────────────────────────────────────────────────────
    struct SubInfo { CLI::App* sub; std::string name; };
    std::vector<SubInfo> clientSubs = {
        {statusSub,       "status"},
        {loadModuleSub,   "load-module"},
        {unloadModuleSub, "unload-module"},
        {reloadModuleSub, "reload-module"},
        {listModulesSub,  "list-modules"},
        {moduleInfoSub,   "module-info"},
        {infoSub,         "info"},
        {callSub,         "call"},
        {moduleSub,       "module"},
        {watchSub,        "watch"},
        {statsSub,        "stats"},
        {stopSub,         "stop"},
        {issueTokenSub,   "issue-token"},
        {revokeTokenSub,  "revoke-token"},
        {listTokensSub,   "list-tokens"},
    };

    for (auto& [sub, name] : clientSubs) {
        if (!sub->parsed())
            continue;

        if (rejectDaemonOnlyFlags())
            return 1;

        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion(QString::fromStdString(logosctl_version::version()));

        // Collect remaining args from the subcommand, extracting global flags
        // (global flags placed after the subcommand end up in remaining())
        std::vector<std::string> cmdArgs;

        for (const auto& r : sub->remaining()) {
            if (r == "--json" || r == "-j") {
                jsonMode = true;
            } else if (r == "--no-json" || r == "--human") {
                humanMode = true;
            } else if (r == "--quiet" || r == "-q") {
                quiet = true;
            } else {
                cmdArgs.push_back(r);
            }
        }

        Output output(jsonMode);
        if (humanMode)
            output.setHumanMode(true);
        RpcClient rpcClient;

        auto cmd = createCommand(name, rpcClient, output);
        if (!cmd) {
            output.printError("INVALID_ARGS",
                              "Unknown command: " + name + ". Run 'logoscore --help' for usage.");
            return 1;
        }

        return cmd->execute(cmdArgs);
    }

    // ── Stray daemon-only flags without -D and without a subcommand ──────────
    if (rejectDaemonOnlyFlags())
        return 1;

    // ── No mode detected — show help ─────────────────────────────────────────
    std::cout << app.help() << std::endl;
    return 0;
}
