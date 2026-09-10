#ifndef DAEMON_STATE_H
#define DAEMON_STATE_H

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <sys/types.h>   // gid_t

#ifdef _WIN32
// mingw-w64's sys/types.h has no gid_t — Windows identifies principals by SID,
// not by a small integer. Declared so the signatures below still compile; the
// only producer, resolveOsGroupGid, always fails on Windows, so no value of
// this type is ever meaningful there.
using gid_t = unsigned int;
#endif

// `daemon/config.json` (operator preferences) and `daemon/state.json`
// (live runtime state). Both use the same v2 schema number — the
// previous unified daemon/daemon.json v2 was internal-only, so the version
// is reused for the split layout.
constexpr int kDaemonConfigSchemaVersion       = 2;
constexpr int kDaemonRuntimeStateSchemaVersion = 2;

// Transport endpoint advertised by the daemon for one module. Clients
// read the per-module `transports` array and pick one (default: local;
// remote clients pick a tcp / tcp_ssl entry by hand or via flags).
struct TransportInfo {
    std::string protocol;   // "local" | "tcp" | "tcp_ssl"
    std::string host;       // bind address (e.g. "0.0.0.0" or "127.0.0.1")
    uint16_t    port = 0;
    // tcp_ssl: CA cert for verification. May appear in state.json
    // because clients need it to verify the server's chain.
    std::string caFile;
    bool        verifyPeer = true;
    // Wire codec for RPC framing (e.g. "json", "cbor"). Only meaningful
    // for plain transports; clients must match what the daemon
    // advertised. Local uses QRO's own format and ignores this.
    std::string codec = "json";
    // tcp_ssl: server reads cert + key from local disk. These are
    // populated only on the daemon side from --ssl-cert / --ssl-key
    // and are deliberately NOT serialized to state.json — clients
    // don't need them, and key paths in particular are local secrets.
    std::string certFile;
    std::string keyFile;
};

// Operator-typed preferences. Persists to daemon/config.json on
// `--persist-config`. Defaults supplied by the merge layer in main.
// Values reflect intent (e.g. `port: 0` stays `0` — the resolved
// port lives in DaemonRuntimeState.resolved).
// Redirects for the session's subdirectories. Empty means "use the default",
// which is <configDir>/<name> -- that default is what makes a session portable.
// Set one to share a keyring between sessions, put the cache on a bigger disk,
// or point at a modules tree something else manages.
struct SessionDirs {
    std::string modules;
    std::string plugins;
    std::string keyring;
    std::string data;
    std::string cache;
    std::string logs;
};

// Daemon log file settings. Capture is pipe-based so module subprocesses are
// included; see src/daemon/log_sink.h.
struct LoggingConfig {
    bool        enabled   = true;
    std::string file      = "daemon.log";
    // Rotate past this size, keeping maxFiles in total. 0 = never rotate.
    std::size_t maxSizeMb = 10;
    std::size_t maxFiles  = 5;
    // Mirror to the terminal too. Ignored once detached, where the original
    // stdout is /dev/null.
    bool console = true;
};

struct DaemonConfig {
    std::vector<std::string> modulesDirs;
    std::string              persistencePath;
    SessionDirs              dirs;
    LoggingConfig            logging;
    // Per-module advertised transport set. Built from
    // `--module-transport` CLI flags (and the previous-launch config
    // when present). `core_service` and `capability_module` always
    // get an entry by the time the merge resolves; the merge layer
    // fills in the LocalSocket-only default when neither operator
    // input nor config.json supplied one.
    std::map<std::string, std::vector<TransportInfo>> modules;
    // Server-side TLS material (paths), from the top-level `ssl:` block.
    // These are session-wide DEFAULTS: every `tcp_ssl` listener that does
    // not name its own `cert:`/`key:`/`ca_file:` inherits them. Applied by
    // `applySslDefaults` below, which the logosctl front-end calls once
    // after loading the config; the per-listener fields are what actually
    // reach the transport set. Stripped from state.json's advertised
    // transport set — the daemon needs them to bind, but clients don't
    // need (and shouldn't see) the key path.
    std::string sslCert;
    std::string sslKey;
    std::string sslCa;
    // Package signature enforcement, pushed into the bundled
    // package_manager module at boot the same way the keyring directory is.
    // One of "none" | "warn" | "require"; empty means "say nothing and let
    // the module keep its own default" (which is warn). Any other value is
    // rejected by the config reader rather than silently ignored.
    std::string signaturePolicy;
    // Mirrors --insecure-tcp. Persisted so operators don't have to
    // retype it next launch. False by default.
    bool insecureTcp = false;
    // Which CONTAINER this daemon's modules must run in: "auto" (or empty,
    // the default) | "inproc" | "subprocess". Pushed into the runtime before
    // any module loads.
    //
    // An ASSERTION rather than a switch, because a module has one artifact and
    // no flag turns a Qt plugin into a Bare module: "inproc" means every module
    // here must BE a Bare module, and a Qt plugin is refused rather than
    // quietly subprocessed. That is what lets a CI job assert the Native
    // container actually ran the thing. See logos_core_set_container_policy.
    std::string container;
    // Inter-module access policy: resolved JSON text (from --access-policy
    // file or inline). Empty means none. Persisted across launches.
    std::string accessPolicy;
    // OS group to share the daemon with (from --access-group). When set, the
    // daemon (1) exports LOGOS_SOCKET_GROUP + LOGOS_SOCKET_MODE=0660 so every
    // module/host process chgrp's + widens its local socket for the group, and
    // (2) makes the client artifacts (client/, client/config.json,
    // client/auto.json) group-readable and chgrp's them, so a second OS user in
    // the group can drive the daemon. Empty means owner-only (the default).
    // Persisted across launches.
    std::string accessGroup;
};

// Live-instance runtime state. Written to daemon/state.json on every
// successful boot (after transports actually bind), removed at clean
// shutdown. Stale state.json after a crash is tolerable: the next
// boot overwrites it; co-resident clients can detect "no live daemon"
// by `kill(state.pid, 0) == ESRCH`.
struct DaemonRuntimeState {
    bool fileOk = false;
    int  schemaVersion = 0;

    // Ephemeral identity for this process.
    std::string instanceId;
    int64_t     pid = -1;
    std::string startedAt;
    // Diagnostic: which layer supplied the highest-precedence value
    // for this run. One of "cli", "config.json", "defaults".
    std::string configSource;

    // Resolved post-merge, post-bind values. Same shape as
    // DaemonConfig but with port=0 replaced by the actually-bound
    // port and any "missing well-known module" gaps filled in.
    DaemonConfig resolved;
};

// Format the current UTC time as ISO 8601 (e.g. "2026-04-28T12:34:56Z").
// Exposed so daemon.cpp can stamp `started_at` without duplicating the
// chrono boilerplate.
std::string currentUtcIso8601();

// Resolve an OS group name-or-numeric-gid to a gid. Returns false on an unknown
// name or an out-of-range numeric value. Shared by the daemon (to validate
// --access-group before exporting the socket-perm env vars) and the client
// artifact writer, so both apply exactly the same policy.
bool resolveOsGroupGid(const std::string& spec, gid_t& out);

// The accepted `signature_policy:` values. Shared by the config reader (which
// rejects anything else) and by callers that want to check a value without
// hard-coding the list.
bool isValidSignaturePolicy(const std::string& policy);

// Push the top-level `ssl:` block down into every `tcp_ssl` listener that did
// not name its own material: `ssl.cert` fills an empty per-listener `cert`,
// `ssl.key` an empty `key`, `ssl.ca` an empty `ca_file`. A listener that names
// its own always wins, so the block is a default and not an override.
//
// Without this the top-level block was parsed, persisted and shown but read by
// nobody, so the obvious way to configure TLS produced listeners with no
// certificate and a handshake that died with "no shared cipher".
//
// logosctl-only: called from src/main.cpp (the Modern front-end). logoscore's
// main_legacy.cpp does not call it, so the legacy tool keeps treating the
// block exactly as it does today.
void applySslDefaults(DaemonConfig& cfg);

// Report every `tcp_ssl` listener still missing a cert or key once the
// defaults above have been applied — such a listener binds without a
// certificate and fails every handshake. Returns one human-readable line per
// offending listener ("core_service tcp_ssl 0.0.0.0:8645"); empty means the
// TLS material is complete. Also logosctl-only, for the same reason.
std::vector<std::string> findTlsListenersMissingMaterial(const DaemonConfig& cfg);

// Validate a daemon-config document (YAML already converted to JSON) and turn
// it into a DaemonConfig, without touching disk. Returns nullopt with a
// one-line reason in `error` for anything the daemon could not boot from: a
// wrong top-level type, an unsupported schema version, a value of the wrong
// type (`modules_dirs:` given a scalar), an out-of-range number, or an invalid
// transport entry. The message names the offending key by its dotted path.
//
// Never throws: a hand-written config must not be able to terminate the
// process. This is the same entry point DaemonConfigFile::read uses, so
// `daemon config set` can validate a document *before* writing it and know
// that anything it accepts is a document the daemon will accept too.
std::optional<DaemonConfig> parseDaemonConfigDocument(const nlohmann::json& obj,
                                                      std::string* error);

// daemon/config.json — operator preferences. read() returns nullopt
// when the file is missing or its schema version doesn't match;
// callers fall through to defaults. write() is atomic (write-temp +
// rename) and creates the parent dir.
class DaemonConfigFile {
public:
    static std::string filePath();
    static std::optional<DaemonConfig> read();
    static bool write(const DaemonConfig& cfg);
};

// daemon/state.json — live runtime state. Written on every boot,
// removed at shutdown. read()'s `fileOk` flag is true iff a daemon
// has announced itself (instance_id populated). remove() is the
// shutdown hook.
class DaemonRuntimeStateFile {
public:
    static std::string filePath();
    static DaemonRuntimeState read();
    static bool write(const DaemonRuntimeState& state);
    static bool remove();

    // Emit <configDir>/client/config.json + <configDir>/client/auto.json
    // populated as a same-host dial spec for the daemon we just bound.
    // Called by the daemon at boot after auto-issuing the `auto` token;
    // allows `logosctl status` (and friends) to work out of the box
    // from the same machine without hand-writing a client config.
    //
    // The emitted client/config.json mirrors the daemon's resolved
    // per-module transports (passed in via `coreServiceTransports` /
    // `capabilityModuleTransports`). For each module, the dial entry
    // is the LocalSocket if present in the resolved set (the common
    // case — `local` is implicitly bound for every module, see
    // `main.cpp`'s `--module-transport` merge), otherwise the first
    // operator-named transport. Wildcard bind addresses ("0.0.0.0",
    // "::") are translated to loopback ("127.0.0.1", "::1") in the
    // client entry so the dial spec is always usable from the same
    // host. daemon/state.json's advertised transport list is unaffected
    // — that one keeps the bind address verbatim because remote
    // clients on a different host need it.
    //
    // Two gates control what gets written:
    //
    //   - The raw client/auto.json is **always** (re)written. The
    //     daemon just (re)issued the auto token; any pre-existing
    //     client/auto.json now holds a stale value, and operators who
    //     reference auto.json from a hand-written client/config.json
    //     deserve to keep working.
    //
    //   - When client/config.json is absent it is (re)generated so a
    //     co-resident client always has a working, current dial spec —
    //     the instance_id changes every boot and this file is the client's
    //     only channel for it, so a persisted config dir that lost the file
    //     (or a second OS user who never had one) must get one back. A
    //     hand-written remote config lives at a path the operator controls;
    //     this only ever (re)creates the daemon's own local dial spec.
    //
    //   - When client/config.json is present it is left untouched so a
    //     hand-written remote-client config is never clobbered — with
    //     one exception: a file carrying an `instance_id` that doesn't
    //     match this daemon is a stale copy of *our own* generated
    //     artifact (a persisted ~/.logosctl whose daemon was replaced,
    //     e.g. a restarting container) and is refreshed in place so
    //     co-resident clients don't dial a dead instance forever. An
    //     operator-authored remote config has no `instance_id`, so it
    //     never matches.
    //
    // For remote operators: don't try to stretch this — copy a
    // daemon/tokens/<name>.json file and hand-write a client config
    // pointing at the daemon's TCP/TLS endpoint. By design.
    //
    // `autoTokenRaw` is the raw token returned from
    // TokenStore::issueToken — written verbatim into client/auto.json
    // so a `cp` operation could replicate it cleanly.
    //
    // When `accessGroup` is non-empty the emitted client dir + files are made
    // group-readable (client/ 0750, config.json / auto.json 0640) and chgrp'd
    // to that group, and the config dir is made group-traversable, so a second
    // OS user in the group can read them. Empty keeps the owner-only default.
    static bool writeLocalClientArtifacts(const std::string& instanceId,
                                          const std::string& autoTokenRaw,
                                          const std::string& issuedAt,
                                          const std::vector<TransportInfo>& coreServiceTransports,
                                          const std::vector<TransportInfo>& capabilityModuleTransports,
                                          const std::string& accessGroup = {});
};

#endif // DAEMON_STATE_H
