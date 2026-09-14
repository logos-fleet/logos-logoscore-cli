# Logosctl CLI Specification

## Overview

The logosctl CLI is the primary interface for operating the Logos Core runtime. It manages the lifecycle of a daemon process that hosts independently developed modules (plugins), and provides commands to load modules, call methods, watch events, and inspect runtime state.

The CLI follows a daemon + client architecture. A long-running daemon process hosts the module runtime, and short-lived client commands connect to it to perform operations.

### Design Goals

1. **Human-friendly** — Readable output, discoverable commands, helpful error messages with recovery suggestions.
2. **Agent-friendly** — Structured JSON output, non-interactive operation, streaming events as NDJSON, deterministic exit codes. An AI agent using a bash tool should be able to operate the full lifecycle without any interactive prompts or ambiguous output.
3. **Composable** — Each command does one thing and works well in pipelines. Output goes to stdout, diagnostics to stderr.
4. **Daemon-oriented** — A long-running daemon owns the modules; clients connect to it. The daemon starts clean (`-m`/`--persistence-path` configure startup with `-D`); modules are loaded via `load-module`.

---

## Architecture

```
                    ┌──────────────────────────┐
                    │     logosctl daemon     │
                    │                          │
                    │  ┌────────────────────┐  │
                    │  │    core_service    │  │
                    │  │ (in-process module)│  │
                    │  └─────────▲──────────┘  │
                    │            │             │
                    │     Qt Remote Objects    │
                    │            │             │
                    └────────────┼─────────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
     ┌────────▼──────┐ ┌──────▼───────┐ ┌──────▼───────┐
     │ logosctl     │ │ logosctl    │ │ logosctl    │
     │ load-module   │ │ call chat   │ │ watch chat   │
     │ waku          │ │ send "hi"   │ │ --event msg  │
     └───────────────┘ └──────────────┘ └──────────────┘
         (exits)          (exits)        (streams)
```

**Daemon** (`logosctl daemon start`):
- Starts the Logos Core runtime and Qt event loop.
- Discovers modules in configured directories.
- Writes `~/.logosctl/daemon/state.json` (live runtime state — instance_id, pid, started_at, resolved transports) on startup, removed on clean shutdown.
- Maintains `~/.logosctl/daemon/tokens.json` (hashed-at-rest accepted-token list — survives restarts).
- Persists `~/.logosctl/daemon/config.json` (operator preferences) only when the operator passed `--persist-config`.
- Auto-issues an `auto` token for the local same-host client and emits `~/.logosctl/client/config.json` + `~/.logosctl/client/auto.json` on the first boot into an empty config dir.

**Client commands** (all other subcommands):
- Read `~/.logosctl/client/config.json` to learn how to dial the daemon and which token file to load.
- Connect to the daemon's `core_service` module via RPC using the token.
- Execute the requested operation, print the result, and exit.
- If no daemon is running, exit with code 2 and a clear error message.

---

## Command Structure

```
logosctl [global-flags] <command> [command-flags] [args...]
```

### Global Flags

| Flag | Short | Description |
|------|-------|-------------|
| `--json` | `-j` | Output as JSON. Default when stdout is not a TTY. |
| `--modules-dir <path>` | `-m` | Module search directory (daemon mode only, repeatable). |
| `--config-dir <path>` | | Override the config directory (default: `~/.logosctl`; also `LOGOSCTL_CONFIG_DIR`). Client commands must pass the same value as the daemon they target. The directory contains the `daemon/` and `client/` subtrees (see [Authentication](#authentication)). |
| `--quiet` | `-q` | Suppress non-essential output. |
| `--verbose` | `-v` | Show debug/info/warning logs (suppressed by default). |
| `--help` | `-h` | Show help. |
| `--version` | | Show version. |

#### Daemon-side transport flags

The daemon defaults to a local Unix socket only for each well-known module
(`core_service`, `capability_module`). To expose either over the network,
pass one or more `--module-transport` flags; each opens an additional
listener that gets advertised in `daemon/state.json`'s `resolved` block.

**Local is always present.** Every module the operator configures (and
the two well-known ones) implicitly gets a LocalSocket listener
prepended to its resolved transport set, in addition to whatever the
operator named via `--module-transport`. The operator's TCP / TCP+SSL
flags add *additional* outside-facing surfaces; they don't replace the
same-host LocalSocket. This keeps the same-host code paths (the
parent's capability_module handshake, the SDK's auto-`requestModule`
flow inside `LogosAPIClient`, cross-module `getClient(name)` calls)
working over LocalSocket regardless of which network transport the
operator chose. `daemon/state.json`'s `resolved.modules.<name>.transports[]`
always lists the LocalSocket entry first, followed by operator-named
entries in the order they were typed.

| Flag | Applies to | Description |
|------|------------|-------------|
| `--module-transport NAME=PROTOCOL[,k=v...]` | daemon | Repeatable. `NAME` is any module the daemon will load (well-known or user-configured). `PROTOCOL` is `local`, `tcp`, or `tcp_ssl`. Each occurrence adds one listener to the named module. If the flag is omitted entirely, every well-known module gets a single `local` listener; if it's passed without a `local` entry for `NAME`, a `local` listener is added implicitly so same-host callers always work. |
| `--insecure-tcp` | daemon | Allow `tcp` (plaintext) listeners on a non-loopback host. Without this flag, the daemon refuses to bind such a listener because tokens travel in cleartext. |

The `k=v` pairs after the protocol configure the listener:

| Key | Used by | Description |
|-----|---------|-------------|
| `host` | tcp, tcp_ssl | Bind address. Defaults to `127.0.0.1` for `tcp`. |
| `port` | tcp, tcp_ssl | Port (`0` = auto-assign). |
| `codec` | tcp, tcp_ssl | Wire codec: `json` (default, debuggable) or `cbor` (compact). |
| `cert` | tcp_ssl | Server cert PEM file. |
| `key` | tcp_ssl | Server private key PEM file. |
| `ca` | tcp_ssl | CA cert PEM file. |
| `verify_peer` | tcp_ssl | `true` / `false` — require client cert verification. |

Each well-known module needs its own listener so the host-side client
can dial each. Examples:

```
# TCP — plaintext, good for localhost or trusted networks. Local
# listeners are added implicitly; just name the TCP one for each
# module that needs an outside-facing surface.
--module-transport core_service=tcp,host=127.0.0.1,port=6000
--module-transport capability_module=tcp,host=127.0.0.1,port=6001

# TCP + TLS — wire-encrypted; cert + key required, CA optional. Local
# listeners are still added implicitly.
--module-transport "core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/p/c.pem,key=/p/k.pem,ca=/p/ca.pem"
--module-transport "capability_module=tcp_ssl,host=0.0.0.0,port=6444,cert=/p/c.pem,key=/p/k.pem,ca=/p/ca.pem"

# Per-module: applies to user modules too. The operator's TCP listener
# is the additional outside-facing surface; same-host callers still
# reach `my_module` over LocalSocket without extra configuration.
--module-transport my_module=tcp,host=127.0.0.1,port=6010
```

#### Client-side dial spec

Client commands never read daemon-only files (`daemon/config.json`,
`daemon/tokens.json`). They read `<configDir>/client/config.json`,
which holds the dial spec (`endpoint`, `host`, `port`, `codec`,
`cert`/`key`/`ca`/`verify_peer` for TLS) and a `token_file` pointing
at the raw-token file alongside. (`status` consults
`daemon/state.json` for a fast same-host liveness check via
`kill(pid, 0)`, but never opens daemon-only secrets.)

The daemon auto-emits `client/config.json` + `client/auto.json` for the
local same-host case on the first boot into an empty config dir — local
clients work out of the box with no manual setup. Subsequent boots leave
an existing `client/config.json` alone (so an operator-written
remote-client config isn't clobbered). For remote clients
(port-forwarded containers, NAT, SSH tunnels) hand-write
`client/config.json` with the right host:port for each module and
reference a `token_file` whose contents was copied from a
`daemon/tokens/<name>.json` on the daemon host.

---

## Commands

### `daemon` / `-D`

Start the daemon process.

```
logosctl daemon start [--modules-dir <path>]...
logosctl daemon [--modules-dir <path>]...
```

Starts the Logos Core runtime in the foreground. Startup and shutdown messages go to stdout (so `> logs.txt` captures them); debug/info/warning logs go to stderr and are suppressed unless `--verbose` is passed. Writes `~/.logosctl/daemon/state.json` on startup (and on the first fresh boot also emits `~/.logosctl/client/config.json` + `~/.logosctl/client/auto.json` for the local client), removes `state.json` on clean shutdown.

The daemon scans the configured module directories for available plugins and makes them available for loading via client commands.

### `load-module <name>`

Load a module into the running daemon.

```
logosctl module load <name>
```

Resolves and loads the named module and all its dependencies. The module must be discoverable in one of the directories configured when the daemon was started.

### `unload-module <name>`

Unload a module from the running daemon.

```
logosctl module unload <name>
```

### `list-modules`

List available or loaded modules.

```
logosctl module ls [--loaded]
```

Without flags, lists all known (discovered) modules. With `--loaded`, lists only currently loaded modules.

Each module has a status: `loaded`, `not_loaded`, `crashed`, or `loading`. When a module has crashed, the output includes uptime (or `-` if not running) and crash metadata is available via `module-info`.

### `status`

Show overall daemon and module health.

```
logosctl daemon status
```

Displays daemon state (PID, uptime, version, instance ID, configured listeners) and a summary of all modules with their status. This is the single "dashboard" command — it shows everything at a glance so agents don't need to chain multiple commands.

When the daemon is not running, exits with code 2 and suggests how to start it.

### `reload-module <name>`

Unload and re-load a module.

```
logosctl module reload <name>
```

Performs an unload followed by a load in a single operation. Useful for recovering crashed modules or picking up configuration changes. If the module is not currently loaded, it falls back to a plain load (rather than erroring), reducing edge cases for agents that just want a module running.

### `module-info <name>`

Show detailed information about a specific module.

```
logosctl module show <name>
```

Displays extended metadata: version, status, dependencies, available methods, emitted events, process info (PID, uptime), and crash details if applicable. Methods and events each carry their `description` (from the module's header doc comments) when documented. This is the deep-inspection counterpart to `list-modules`.

### `call <module> <method> [args...]`

Call a method on a loaded module.

```
logosctl call <module> <method> [args...]
```

Invokes the named method on the specified module. Arguments are positional. Use the `@file` prefix to read a parameter value from a file.

Arguments are automatically type-coerced: numeric strings become integers or doubles, `"true"`/`"false"` become booleans, and everything else remains a string. This allows method signatures with typed parameters to match correctly.

```bash
logosctl call chat send_message "hello"
logosctl call storage load_config @config.json
logosctl call math twoArgs "hello" 2          # "hello" as string, 2 as integer
logosctl call config setBool "flag" true       # "flag" as string, true as boolean
```

**Alternative syntax** (explicit form for readability):

```bash
logosctl module <name> method <method> [args...]
logosctl module chat method send_message "hello"
```

Both forms are equivalent. `call` is the short form; `module ... method ...` is the verbose form.

### `watch <module> [--event <name>]`

Watch events from a loaded module.

```
logosctl watch <module> [--event <name>]
```

Streams events to stdout as they arrive. Without `--event`, streams all events from the module. Runs until interrupted (SIGINT / SIGTERM).

```bash
logosctl watch chat --event chat-message
logosctl watch chat --event chat-message >> events.log &
logosctl watch chat --event chat-message --json | jq .
```

### `stats`

Show resource usage for loaded modules.

```
logosctl stats
```

Displays CPU and memory usage for each loaded module process.

### `stop`

Stop the running daemon.

```
logosctl daemon stop
```

If `daemon/state.json` records this client's daemon instance with a pid that is no longer alive, the session is stale: `stop` reports `NO_DAEMON` and exits 2 without dialing anything. This is not specific to `stop` — see [No daemon running](#no-daemon-running) — but it matters most here, because the shutdown path below reads a missing reply as a successful shutdown.

Otherwise it sends a shutdown request to the daemon via `core_service`. The daemon performs a clean shutdown: unloads all modules, removes `daemon/state.json`, and exits. The client prints a confirmation message and exits.

If the daemon exits before the RPC response arrives (expected behavior), the client treats the missing reply as a successful shutdown — but only after confirming that the daemon really is gone, by watching the pid recorded in `daemon/state.json` (or, for a remote daemon, by re-probing the endpoint). A daemon that neither answers nor exits is reported as an error, not quietly accepted. When the confirmation path is used, the JSON form carries an extra `"confirmed_by"` field naming the evidence (`process-exit` or `endpoint-unreachable`); when the reply arrives normally it is absent.

`LOGOSCTL_SHUTDOWN_GRACE_MS` (daemon-side, default `200`) sets how long the daemon waits after answering before it leaves its event loop. It is a margin, not a correctness mechanism — the daemon drains the transport before quitting regardless — so `0` is a legal setting, and is what the shutdown regression test uses.

**Human:**
```
$ logosctl daemon stop
Daemon stopped.
```

**JSON:**
```json
$ logosctl daemon stop --json
{"status":"ok","message":"Daemon shutting down."}
```

### `info <module>`

Alias for `module-info <module>`. See `module-info` above for full details.

```
logosctl module show <module>
```

Displays version, dependencies, available methods, and crash details (if applicable) for the named module.

### `issue-token --name <name>`

Issue a new named token and write it to `<configDir>/daemon/tokens/<name>.json`.

```
logosctl token issue --name <name> [--replace] [--expires <dur>] [--local-only]
```

Appends an entry to `<configDir>/daemon/tokens.json["tokens"]` (a
`{name, hash, issued_at, expires_at, local_only}` row, hashes are SHA-256
hex) and writes a companion raw-value file at `daemon/tokens/<name>.json`
for distribution. Without `--replace`, the command refuses to overwrite an
existing token with the same name so a stale credential isn't silently
invalidated; pass `--replace` to rotate.

`--expires <dur>` sets a TTL after which the daemon rejects the token (e.g.
`30d`, `12h`). `--local-only` marks the token as valid only over LocalSocket,
so even a compromised TCP listener can't replay it.

After copying `daemon/tokens/<name>.json` to the client host (typically into
the client's `<configDir>/client/`), the operator may delete the daemon-side
raw file — the daemon validates against the in-memory map seeded from
`tokens.json["tokens"]`'s hashes, not the raw file. Distribute the raw file
the way you'd distribute a private key; do not commit it to version control.

This command operates directly on the config dir on disk; it doesn't need the
daemon to be running. Operator-issued tokens take effect on the next daemon
restart (SIGHUP-driven reload is a follow-up).

### `revoke-token <name>`

Remove a named token from `<configDir>/daemon/tokens.json["tokens"]`.

```
logosctl token revoke <name>
```

After this returns, any RPC presenting the revoked token is rejected by the
daemon with an authentication error. The on-disk
`daemon/tokens/<name>.json` file is also removed so clients that still have
it can't mistake it for valid.

### `list-tokens`

List all tokens currently issued against this config dir.

```
logosctl token ls
```

Shows token name, issued-at timestamp, expires-at, and the local-only flag —
never the plaintext token, which only lives in the
`daemon/tokens/<name>.json` file at the moment of issuance. Lost a token?
Rotate it with `issue-token --replace`.

---

## Authentication

### How Tokens Work

Logos Core uses UUID-based tokens for authentication. Every module loaded into the runtime receives a unique token generated by the core. These tokens are used to authorize RPC calls between components.

The CLI needs a token to authenticate with the daemon's `core_service`. This token is called the **client token** and is generated by the daemon on startup.

### Token Lifecycle

```
1. DAEMON STARTS
   logosctl daemon start --detach
   → Daemon mints an "auto" token (local_only=true) for the local client
   → Hash + metadata persisted into ~/.logosctl/daemon/tokens.json["tokens"]
   → Raw value emitted to ~/.logosctl/client/auto.json
   → ~/.logosctl/client/config.json written so local clients dial correctly

2. CLIENT CONNECTS
   logosctl module load waku
   → Reads ~/.logosctl/client/config.json (dial spec + token_file)
   → Loads the raw token from the file token_file points at
   → Sends token with RPC request to `core_service`
   → `core_service` validates the token's hash against tokens.json["tokens"]
   → Request authorized, module loads

3. REMOTE / PROGRAMMATIC ACCESS
   LOGOSCTL_TOKEN=<token> logosctl module load waku
   → Token from env var overrides the one in client/config.json's token_file
   → Useful when client/ isn't writable (remote, containers, CI)
```

### Token Resolution Order

When a client command runs, the token is resolved in this order (first match wins):

| Priority | Source | Example |
|----------|--------|---------|
| 1 | `LOGOSCTL_TOKEN` env var | `LOGOSCTL_TOKEN=abc123 logosctl module ls` |
| 2 | `<configDir>/client/<token_file>` | the path is whatever `client/config.json` says (defaults to `auto.json`) |

A named token issued by `logosctl token issue --name alice` produces
`<configDir>/daemon/tokens/alice.json` on the daemon host. To use it as a
client on a different machine, copy the file into the client host's
`<configDir>/client/` and reference it via `token_file` in `client/config.json`.
Once copied, the operator may delete the daemon-side raw file — validation
keeps working because the hash is what the daemon checks.

### Obtaining a Token

**Local usage (same machine):** No manual token management needed. At boot
the daemon auto-issues an `auto` token (with `local_only=true`, so it can't
be used over TCP), writes the hash into `daemon/tokens.json["tokens"]`, and
emits the raw value into `client/auto.json` alongside a local-default
`client/config.json`. Local client commands just work.

**Remote or programmatic usage:** Issue a named token on the daemon host and
move it to the client host:

```bash
# On the machine running the daemon:
logosctl token issue --name alice
cat ~/.logosctl/daemon/tokens/alice.json
# Output: 550e8400-e29b-41d4-a716-446655440000

# On the remote machine or in a script:
export LOGOSCTL_TOKEN=550e8400-e29b-41d4-a716-446655440000
logosctl module ls --json

# Or persist by copying the file alongside a hand-written client/config.json:
mkdir -p ~/.logosctl/client
scp daemon-host:~/.logosctl/daemon/tokens/alice.json ~/.logosctl/client/alice.json
# then edit ~/.logosctl/client/config.json so token_file = "alice.json"
```

**CI / containers:** Pass the token as an environment variable at runtime:

```bash
docker run -e LOGOSCTL_TOKEN=$TOKEN myimage logosctl module ls --json
```

### Daemon files (config / state / tokens)

The daemon dir splits by lifetime into three files:

- **`daemon/state.json`** — live runtime state. Written every boot
  (after transports actually bind), removed on clean shutdown.
- **`daemon/config.json`** — operator preferences. Written ONLY when
  the operator passed `--persist-config`; otherwise absent.
- **`daemon/tokens.json`** — hashed-at-rest accepted-token list.
  Independent of the running daemon's lifetime.

#### `daemon/state.json`

```json
{
  "version": 2,
  "instance_id": "a3f1c8d20b4e",
  "pid": 12345,
  "started_at": "2026-03-23T14:00:00Z",
  "config_source": "cli",
  "resolved": {
    "modules_dirs": ["/path/to/modules"],
    "persistence_path": "/var/lib/logosctl",
    "modules": {
      "core_service": {
        "transports": [
          { "protocol": "local" },
          { "protocol": "tcp",     "host": "0.0.0.0", "port": 6000, "codec": "json" },
          { "protocol": "tcp_ssl", "host": "0.0.0.0", "port": 6443,
            "codec": "cbor", "ca_file": "/etc/logosctl/ca.pem",
            "verify_peer": true }
        ]
      },
      "capability_module": {
        "transports": [
          { "protocol": "local" },
          { "protocol": "tcp", "host": "127.0.0.1", "port": 6001, "codec": "json" }
        ]
      }
    },
    "ssl": { "cert": "", "key": "", "ca": "" },
    "insecure_tcp": false
  }
}
```

- `instance_id` is a 12-char UUID prefix the client uses with `LogosInstance::id()`
  to reconstruct the same registry URL the daemon published (`local:logos_core_service_<id>`).
- `pid` lets co-resident clients detect a stale state file
  (`kill(pid, 0) == ESRCH` after a hard crash).
- `config_source` records where the running daemon's config came from:
  `cli` (any `--module-transport`/`--insecure-tcp`/etc. flag was
  passed), `config.json` (loaded from disk only), or `defaults`.
- `resolved.modules` is the post-bind transport set: `port: 0` in
  config.json becomes the actually-bound port here.
- `resolved` mirrors the shape of `daemon/config.json` (same field set,
  minus `version`).

#### `daemon/config.json` (operator preferences)

Same shape as `state.json`'s `resolved` block, plus `version`. Reflects
*operator intent* — `port: 0` stays `0` (auto-pick) — not the resolved
post-bind values. Written only when `--persist-config` is passed.

#### `daemon/tokens.json`

```json
{
  "version": 2,
  "tokens": [
    { "name": "auto",  "hash": "<sha256-hex>", "issued_at": "...", "expires_at": null, "local_only": true },
    { "name": "alice", "hash": "<sha256-hex>", "issued_at": "...", "expires_at": "...", "local_only": false }
  ]
}
```

One entry per issued token: `{name, hash, issued_at, expires_at,
local_only}`. Hashes are SHA-256 hex; raw values live only in
`daemon/tokens/<name>.json` at issue time. Independent of the running
daemon's lifetime — survives restarts.

These three files are daemon-owned; the client never reads
`config.json` or `tokens.json`, and only consults `state.json` for a
fast same-host liveness check via `kill(pid, 0)`. The client reads
`<configDir>/client/config.json` to learn how to dial. Liveness — *is
the daemon actually answering?* — falls through to the first RPC (e.g.
`status`), so a connect failure surfaces via the same code path as any
other method call.

---

## Output Design

Every command produces output in one of two modes: human (default when stdout is a TTY) or JSON (when `--json` is passed or stdout is piped/redirected).

### `load-module`

**Human:**
```
$ logosctl module load waku
Loaded module: waku (v0.1.0)
  Dependencies loaded: store
```

**JSON:**
```
$ logosctl module load waku --json
{"status":"ok","module":"waku","version":"0.1.0","dependencies_loaded":["store"]}
```

**Error (human):**
```
$ logosctl module load nonexistent
Error: Module 'nonexistent' not found.
  Known modules: waku, chat, delivery, store
  Scan additional directories with: logosctl daemon start -m /path/to/modules
```

**Error (JSON):**
```
$ logosctl module load nonexistent --json
{"status":"error","code":"MODULE_NOT_FOUND","message":"Module 'nonexistent' not found.","known_modules":["waku","chat","delivery","store"]}
```

### `unload-module`

**Human:**
```
$ logosctl module unload waku
Unloaded module: waku
```

**JSON:**
```
$ logosctl module unload waku --json
{"status":"ok","module":"waku"}
```

### `list-modules`

**Human:**
```
$ logosctl module ls
NAME        VERSION   STATUS      UPTIME
waku        v0.1.0    loaded      2h 14m
chat        v0.2.0    crashed     -
delivery    v0.1.0    not loaded  -
store       v0.3.0    loaded      2h 14m

$ logosctl module ls --loaded
NAME        VERSION   STATUS    UPTIME
waku        v0.1.0    loaded    2h 14m
store       v0.3.0    loaded    2h 14m
```

**JSON:**
```json
$ logosctl module ls --json
[
  {"name":"waku","version":"0.1.0","status":"loaded","uptime_seconds":8040},
  {"name":"chat","version":"0.2.0","status":"crashed","exit_code":139,"crashed_at":"2026-03-23T14:22:01Z","crash_reason":"SIGSEGV"},
  {"name":"delivery","version":"0.1.0","status":"not_loaded"},
  {"name":"store","version":"0.3.0","status":"loaded","uptime_seconds":8040}
]
```

Note: the `status` field is an enum of `loaded | not_loaded | crashed | loading`. Crash metadata (`exit_code`, `crashed_at`, `crash_reason`) only appears when status is `crashed` — the JSON doesn't bloat clean entries with null crash fields.

### `status`

**Human:**
```
$ logosctl daemon status
Logosctl Daemon
  Status:       running
  PID:          12847
  Uptime:       4h 32m
  Version:      v0.5.0
  Instance ID:  a3f1...c8d2
  State file:   /Users/iuri/.logosctl/daemon/state.json

Modules: 3 loaded, 1 crashed, 1 not loaded
  waku        v0.1.0    loaded      2h 14m
  chat        v0.2.0    crashed     -
  delivery    v0.1.0    not loaded  -
  store       v0.3.0    loaded      4h 32m
  payments    v0.1.0    loaded      4h 32m
```

**JSON:**
```json
$ logosctl daemon status --json
{
  "daemon": {
    "status": "running",
    "pid": 12847,
    "version": "0.5.0"
  },
  "modules_summary": {
    "loaded": 3,
    "crashed": 1,
    "not_loaded": 1
  },
  "modules": [
    {"name":"waku","version":"0.1.0","status":"loaded","uptime_seconds":8040},
    {"name":"chat","version":"0.2.0","status":"crashed","exit_code":139,"crashed_at":"2026-03-23T14:22:01Z"},
    {"name":"delivery","version":"0.1.0","status":"not_loaded"},
    {"name":"store","version":"0.3.0","status":"loaded","uptime_seconds":16320},
    {"name":"payments","version":"0.1.0","status":"loaded","uptime_seconds":16320}
  ]
}
```

**When daemon is not running:**
```
$ logosctl daemon status
Logosctl Daemon
  Status:       not running

No daemon state file at /Users/iuri/.logosctl/daemon/state.json
Run "logosctl daemon start" to start the daemon.

$ echo $?
1
```

```json
$ logosctl daemon status --json
{
  "daemon": {
    "status": "not_running"
  }
}
$ echo $?
1
```

### `reload-module`

**Human:**
```
$ logosctl module reload chat
Unloading chat...  done
Loading chat...    done
Module "chat" reloaded successfully (v0.2.0, pid 51203)
```

**JSON:**
```json
$ logosctl module reload chat --json
{
  "action": "reload",
  "module": "chat",
  "version": "0.2.0",
  "status": "loaded",
  "pid": 51203,
  "previous_status": "crashed",
  "duration_ms": 340
}
```

**When reload fails:**
```
$ logosctl module reload chat
Unloading chat...  done
Loading chat...    failed

Error: module "chat" failed to start (exit code 1)
  Last log: "Config file not found: /etc/logosctl/chat.toml"

Run "logosctl module-logs chat --tail 20" for details.

$ echo $?
3
```

```json
$ logosctl module reload chat --json
{
  "action": "reload",
  "module": "chat",
  "status": "error",
  "error": "module failed to start",
  "exit_code": 1,
  "last_log_line": "Config file not found: /etc/logosctl/chat.toml"
}
$ echo $?
3
```

**Reload a module that isn't loaded (behaves like load):**
```
$ logosctl module reload delivery
Module "delivery" is not loaded. Loading...
Loading delivery...  done
Module "delivery" loaded successfully (v0.1.0, pid 51210)
```

### `module-info`

**Human:**
```
$ logosctl module show chat
Name:          chat
Version:       v0.2.0
Status:        loaded
PID:           23457
Uptime:        2h 14m
Dependencies:  waku, store

Methods:
  send_message(text: QString) -> QString
      Sends a chat message to the active channel.
  get_history() -> QJsonArray
      Returns the message history for the active channel.
  set_nickname(name: QString) -> bool
  get_status() -> QString

Events:
  message_received(from: QString, body: QString)
      Emitted when a new message arrives on the active channel.
  connection_changed(online: bool)
```

Each method line shows `name(param: type, …) -> returnType`. When a method
carries documentation, its `description` is printed on the following line(s),
indented — a multi-line doc comment keeps its line breaks, one indented line
each. The description originates from the doc comment written directly above the
method's declaration in the module's header (see the module-builder docs);
methods without a doc comment simply omit it.

The **Events** section lists the events the module emits, in the same
`name(param: type, …)` form — but with no return type, since events are
fire-and-forget. An event's `description` (from the doc comment above its
`logos_events:` declaration) is printed indented beneath it, exactly as for
methods. The section is omitted when the module declares no events.

**Crashed module:**
```
$ logosctl module show chat
Name:          chat
Version:       v0.2.0
Status:        crashed
Exit Code:     139 (SIGSEGV)
Crashed At:    2026-03-23T14:22:01Z
Restart Count: 3
Last Log:      "Segmentation fault in message_handler.cpp:142"
```

**JSON:**
```json
$ logosctl module show chat --json
{
  "name": "chat",
  "version": "0.2.0",
  "status": "loaded",
  "pid": 23457,
  "uptime_seconds": 8040,
  "dependencies": ["waku", "store"],
  "methods": [
    {"name": "send_message", "signature": "send_message(QString)", "returnType": "QString", "isInvokable": true, "description": "Sends a chat message to the active channel.", "parameters": [{"name": "text", "type": "QString"}]},
    {"name": "get_history", "signature": "get_history()", "returnType": "QJsonArray", "isInvokable": true, "description": "Returns the message history for the active channel.", "parameters": []},
    {"name": "set_nickname", "signature": "set_nickname(QString)", "returnType": "bool", "isInvokable": true, "parameters": [{"name": "name", "type": "QString"}]},
    {"name": "get_status", "signature": "get_status()", "returnType": "QString", "isInvokable": true, "parameters": []}
  ],
  "events": [
    {"name": "message_received", "signature": "message_received(QString,QString)", "description": "Emitted when a new message arrives on the active channel.", "parameters": [{"name": "from", "type": "QString"}, {"name": "body", "type": "QString"}]},
    {"name": "connection_changed", "signature": "connection_changed(bool)", "parameters": [{"name": "online", "type": "bool"}]}
  ]
}
```

The `methods` array is the module's `getPluginMethods` introspection, emitted
verbatim. Each entry carries `name`, `signature`, `returnType`, `isInvokable`,
`parameters` (each `{name, type}`), and — when the method is documented —
`description` (sourced from the method's header doc comment).

The `events` array is the module's `getPluginEvents` introspection. Each entry
carries `name`, `signature`, `parameters` (each `{name, type}`), and — when the
event is documented — `description`. There is no `returnType`/`isInvokable`:
events are void. Modules with no declared events report an empty array — legacy
Q_INVOKABLE modules (`interface: "legacy"`) always do, since they have no
`logos_events:` section for the introspection to read. (This used to say
"legacy `provider` modules"; `interface: "provider"` is no longer a buildable
module kind — the module builder refuses it and points at
`interface: "universal"`.)

**Crashed module (JSON):**
```json
$ logosctl module show chat --json
{
  "name": "chat",
  "version": "0.2.0",
  "status": "crashed",
  "exit_code": 139,
  "crash_signal": "SIGSEGV",
  "crashed_at": "2026-03-23T14:22:01Z",
  "restart_count": 3,
  "last_log_line": "Segmentation fault in message_handler.cpp:142",
  "pid_before_crash": 48291
}
```

### `call`

**Human:**
```
$ logosctl call chat send_message "hello world"
message sent (id: msg_4a7b2c)

$ logosctl call math add 2 3
5
```

In human mode, scalar results (strings, numbers, booleans) are printed as plain values. Structured results (objects, arrays) are printed as indented JSON. Null results produce no output.

**JSON:**
```
$ logosctl call chat send_message "hello world" --json
{"status":"ok","module":"chat","method":"send_message","result":"message sent (id: msg_4a7b2c)"}
```

When the method returns structured data:
```
$ logosctl call chat get_history --json
{"status":"ok","module":"chat","method":"get_history","result":[{"id":"msg_4a7b2c","from":"alice","text":"hello","timestamp":"2026-03-23T14:30:01Z"},{"id":"msg_5d8e3f","from":"bob","text":"hi there","timestamp":"2026-03-23T14:30:05Z"}]}
```

**LogosResult return values:**

Methods declared to return `LogosResult` (the common ok/error wrapper) are
serialised as:

```json
{"success": <bool>, "value": <any>, "error": <any>}
```

`value` is whatever the method stuffed in on success; `error` is whatever it
stuffed in on failure; the unused side is `null`. Same shape regardless of
whether the daemon-module hop went over the local socket (QRO), TCP, or
TCP+SSL — pick the transport you like, assertions stay identical.

```
$ logosctl call account create_account --json
{"status":"ok","module":"account","method":"create_account",
 "result":{"success":true,"value":{"id":"42","name":"alice"},"error":null}}

$ logosctl call account create_account --json    # duplicate name
{"status":"ok","module":"account","method":"create_account",
 "result":{"success":false,"value":null,"error":"name already taken"}}
```

**Error (human):**
```
$ logosctl call chat nonexistent_method
Error: Method 'nonexistent_method' not found on module 'chat'.
  Available methods: send_message, get_history, set_nickname, get_status
```

**Error (JSON):**
```
$ logosctl call chat nonexistent_method --json
{"status":"error","code":"METHOD_NOT_FOUND","message":"Method 'nonexistent_method' not found on module 'chat'.","available_methods":["send_message","get_history","set_nickname","get_status"]}
```

**Failed call (JSON):**
```
$ logosctl call chat send_message --json
{"status":"error","code":"METHOD_FAILED","message":"Call to chat.send_message failed (unauthorized: token not recognized).","error":{"code":"unauthorized","message":"token not recognized","origin":"chat"}}
```

`METHOD_FAILED` covers every failure the transport itself detected, and `error`
carries which one, verbatim from the protocol's call-error vocabulary:
`object_unavailable`, `timeout`, `transport_error`, `call_failed`,
`unauthorized`, plus the codes a provider that RAN and refused answers as its
result rather than on the error channel: `dispatch_failed` (it refused the
argument VALUES) and `invalid_args` (wrong argument COUNT). `unknown_method` is
recognised too, ahead of any provider emitting it. `argument_mismatch` is the
one code `call` raises itself — see the `null` paragraph below.

That in-band set is CLOSED, deliberately. A method may legitimately return a
`{code, message, origin}` map of its own; matching the shape rather than the
code would turn its data into an error. Anything outside the set comes back as
`"result"`.

> **This changed.** `invalid_args` used to be matched by nobody, so
> `logosctl call test_basic_module isPositive` with the argument missing exited
> **0** with `status: "ok"` and the refusal object as its `result`. It now exits
> **4** with `METHOD_FAILED` and `error.code: "invalid_args"`.

A result of `null` is **not** a failure. It is a value — an empty optional, or a
method that returns nothing in particular — and reports `status: "ok"` with
`"result": null`. Two things `null` cannot express on its own, and `call`
resolves both by asking the module for its own interface — one extra round-trip,
paid on a null return and nowhere else:

* **an unknown method name**, which no provider distinguishes on the wire. That
  is where `METHOD_NOT_FOUND` above comes from.
* **an argument the declared parameter cannot take.** The published parameter
  types say so, and the envelope then reports `METHOD_FAILED` with
  `error.code: "argument_mismatch"` — the one code in the set that no provider
  ever said:

```
$ logosctl call keystore_module has_address 42 --json
{"status":"error","code":"METHOD_FAILED","message":"Call to keystore_module.has_address failed (argument_mismatch: expected string at arg0, got number).","error":{"code":"argument_mismatch","message":"expected string at arg0, got number","origin":"keystore_module"}}
```

> **This changed.** Such a call used to reach the module, fail to match any
> signature there, and come back `{"result": null, "status": "ok"}` — exit code
> **0**, nothing logged by the daemon, and a script reading the exit code saw
> success. It now exits **4**.

The check is deliberately narrow, because a false positive would refuse a call
that works: it speaks only when the module published a parameter list, the
argument COUNT already agrees (a count is the provider's `invalid_args` to
report), the declared type has an unambiguous JSON counterpart (`any` and every
`?T` publish as `QVariant`, which accepts anything), and the value is not
`null`. Anything else stays `status: "ok"`.

**Timeout error (JSON):**
```
$ logosctl call chat slow_operation --json
{"status":"error","code":"TIMEOUT","message":"Call to chat.slow_operation timed out after 30s."}
```

> `TIMEOUT` is not yet emitted by `call`: a deadline that elapses arrives as
> `METHOD_FAILED` with `error.code == "timeout"`.

### `watch`

Streams continuously until interrupted. Each event is printed as it arrives.

**Human:**
```
$ logosctl watch chat --event chat-message
[14:30:01] chat :: chat-message
  from: alice
  text: hello world

[14:30:05] chat :: chat-message
  from: bob
  text: hi there

[14:31:12] chat :: chat-message
  from: alice
  text: how are you?
^C
```

**JSON (NDJSON — one self-contained JSON object per line):**
```
$ logosctl watch chat --event chat-message --json
{"timestamp":"2026-03-23T14:30:01Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"hello world"}}
{"timestamp":"2026-03-23T14:30:05Z","module":"chat","event":"chat-message","data":{"from":"bob","text":"hi there"}}
{"timestamp":"2026-03-23T14:31:12Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"how are you?"}}
```

All events from a module (no `--event` filter):
```
$ logosctl watch chat --json
{"timestamp":"2026-03-23T14:30:01Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"hello"}}
{"timestamp":"2026-03-23T14:30:02Z","module":"chat","event":"user-joined","data":{"user":"bob"}}
{"timestamp":"2026-03-23T14:30:05Z","module":"chat","event":"chat-message","data":{"from":"bob","text":"hi"}}
{"timestamp":"2026-03-23T14:30:06Z","module":"chat","event":"typing","data":{"user":"alice"}}
```

### `stats`

**Human:**
```
$ logosctl stats
MODULE      PID     CPU%    MEMORY
waku        23456   2.1%    48.3 MB
chat        23457   0.4%    22.1 MB
store       23458   0.1%    15.7 MB
```

**JSON:**
```
$ logosctl stats --json
[
  {"name":"waku","pid":23456,"cpu_percent":2.1,"memory_mb":48.3},
  {"name":"chat","pid":23457,"cpu_percent":0.4,"memory_mb":22.1},
  {"name":"store","pid":23458,"cpu_percent":0.1,"memory_mb":15.7}
]
```

### `info`

Alias for `module-info`. See the `module-info` output section above for all output examples including human, JSON, and crashed module variants.

### No daemon running

**Human:**
```
$ logosctl module ls
Error: No running logosctl daemon.
  Start one with: logosctl daemon start
  Start with modules: logosctl daemon start -m /path/to/modules
```

**JSON:**
```
$ logosctl module ls --json
{"status":"error","code":"NO_DAEMON","message":"No running logosctl daemon. Start one with: logosctl daemon start"}
```

#### A session whose daemon is gone

A session directory can outlive its daemon, and from the client's side it still
looks dialable: the dial spec and token are right where they were. Such a
session is refused **up front**, before anything is dialed. Every command whose
first act is an RPC applies the check: `call`, `catalog`, `key`, `module ls` /
`load` / `show` / `reload` / `unload`, `package` (all subcommands), `stats`,
`stop`, `watch`.

The check exists because connecting proves nothing. A LocalSocket client
succeeds against a socket path with no listener, and QtRO reports nothing for
an absent peer, so the request is sent, never answered and never refused — and
the command waits out the full RPC deadline (20s) before reporting a failure
the session could have named immediately.

It comes in two shapes, settled by different evidence:

**The daemon crashed.** `daemon/state.json` is still there, naming a pid that
is no longer alive.

```
$ logosctl module ls --json
{"status":"error","code":"NO_DAEMON","message":"No daemon running (stale state file: pid 51203 is gone)."}
```

**The daemon was stopped.** A clean shutdown *removes* `daemon/state.json`, so
there is no pid to check. What settles it is the socket the dial resolves to:
either it is not there (a clean stop unlinks it) or it is there and refuses the
connection (a hard kill leaves the file behind, and so does a clean stop for
the moment between its reply and its teardown).

```
$ logosctl module ls --json
{"status":"error","code":"NO_DAEMON","message":"No daemon running (no local endpoint at /tmp/logos_core_service_a1b2c3d4e5f6). A daemon that stopped removes it; start one in this session to get it back."}
```

Both are exit 2, and both are immediate.

Cases that deliberately fall through to a normal dial rather than being
refused: a `tcp` / `tcp_ssl` dial (no local socket to look for, and no local
pid either), a dial spec with no `instance_id`, a `daemon/state.json` whose
`instance_id` does not match the client's, and a socket that accepts the
connection. The `instance_id` ones are what keep a *remote* client working when
a co-resident daemon has left its own state file behind — that pid says nothing
about the daemon at the far end of a TCP connection, and a remote dial spec
carries no `instance_id`, so it never matches.

`status` asks the same question and answers it as a status report instead of an
error — `{"daemon":{"status":"not_running","reason":"stale state file: pid
51203 is gone","pid":51203}}`, exit 1 — as a single JSON document.

### Output Rules

- Primary output (results, data) goes to stdout.
- Debug, info, and warning logs go to stderr and are suppressed by default. Pass `--verbose` to show them.
- Critical and fatal errors always go to stderr.
- In JSON mode, colors are disabled and only structured data goes to stdout.
- JSON mode auto-activates when stdout is not a TTY (piped or redirected), so agents and scripts get JSON by default without needing `--json`.
- Daemon startup/shutdown messages go to stdout, so `logosctl daemon start > logs.txt` captures them correctly.

---

## Error Handling

### Exit Codes

| Code | Meaning | When |
|------|---------|------|
| `0` | Success | Operation completed |
| `1` | General error | Unexpected failure, invalid arguments |
| `2` | Connection error | No daemon running, daemon unreachable |
| `3` | Module error | Module not found, failed to load/unload |
| `4` | Method error | Method not found, invocation failed, timeout |

### JSON Error Envelope

All errors in JSON mode follow this structure:

```json
{
  "status": "error",
  "code": "ERROR_CODE",
  "message": "Human-readable description with recovery suggestion."
}
```

Error codes: `NO_DAEMON`, `DAEMON_UNREACHABLE`, `MODULE_NOT_FOUND`, `MODULE_LOAD_FAILED`, `MODULE_NOT_LOADED`, `METHOD_NOT_FOUND`, `METHOD_FAILED`, `TIMEOUT`, `AUTH_FAILED`, `INVALID_ARGS`.

**An unanswered query is never reported as an empty result.** `module ls` and
`stats` return `DAEMON_UNREACHABLE` with exit 2 when the RPC produced no reply,
rather than printing `[]` and exiting 0 — a caller cannot tell that apart from
a healthy session with nothing loaded. `status` likewise exits 1, not 0, when
its report is the synthesized "not running" one.

---

## Daemon + client workflow

Module method calls go through a running daemon. Start a **clean** daemon with
`-D` (it loads no modules on its own), then load modules and call methods with
client subcommands:

```bash
# Start a clean daemon scanning /path
logosctl daemon start -m /path &
logosctl module load waku          # deps resolved automatically
logosctl module load chat
logosctl call chat send_message "hello"
```

The legacy inline mode (`-c "module.method(args)"` / `--quit-on-finish`, which
started the core, ran calls in one short-lived process, and exited) has been
removed, as has `-l/--load-modules` (the daemon now starts clean — load via
`load-module`). `-m`/`--persistence-path` apply only to daemon startup (`-D`);
a subcommand operates in client mode and connects to a running daemon.

---

## AI Agent Workflow

This section describes how an AI agent (such as Claude Code, Cursor, or similar tools that execute bash commands via a tool-use interface) would interact with the logosctl CLI.

### How Agents Use This CLI

AI agents interact with CLIs by executing bash commands and parsing stdout. They cannot handle interactive prompts, colored output, or ambiguous formatting. The logosctl CLI is designed for this:

- **JSON by default when piped.** Since agents capture stdout programmatically (not via a TTY), JSON mode activates automatically. No need to remember `--json`.
- **Deterministic exit codes.** Agents check `$?` after each command to decide whether to proceed or handle an error. Each error category has a distinct code.
- **Structured errors.** When something fails, the JSON error includes a `code` field the agent can branch on, and a `message` field with recovery instructions the agent can follow.
- **No interactive prompts.** Every operation completes without requiring user input.
- **Self-describing.** `logosctl module show <module> --json` tells the agent what methods are available and what parameters they take, without needing external documentation.

### Example: Agent Preflight — Health Check Before Doing Work

Before performing any operation, an agent checks daemon health and ensures required modules are running:

```bash
# Step 1: Is the daemon alive?
if ! logosctl daemon status --json | jq -e '.daemon.status == "running"' > /dev/null 2>&1; then
  echo "daemon not running, starting..."
  logosctl daemon start --detach &
  sleep 2
fi

# Step 2: Check if the module I need is healthy
MODULE_STATUS=$(logosctl daemon status --json | jq -r '.modules[] | select(.name=="chat") | .status')

case "$MODULE_STATUS" in
  "loaded")     echo "ready" ;;
  "crashed")    logosctl module reload chat ;;
  "not_loaded") logosctl module load chat ;;
  *)            echo "unknown state: $MODULE_STATUS" ; exit 1 ;;
esac
```

### Example: Agent Detects and Recovers a Crashed Module

```bash
# Agent checks module health
STATUS=$(logosctl module ls --json | jq -r '.[] | select(.name=="chat") | .status')

if [ "$STATUS" = "crashed" ]; then
  # Get crash details for decision-making
  CRASH_INFO=$(logosctl module show chat --json)
  RESTARTS=$(echo "$CRASH_INFO" | jq '.restart_count')

  if [ "$RESTARTS" -lt 5 ]; then
    logosctl module reload chat
  else
    echo "chat module crashed $RESTARTS times, escalating"
    # agent decides to alert or investigate logs
    logosctl module-logs chat --tail 50
  fi
fi
```

### Example: Agent Builds and Tests a Chat Application

This is a realistic sequence an AI agent would execute when asked to "set up and test the chat module":

```bash
# Step 1: Start the daemon and verify it's running
logosctl daemon start --detach &
sleep 2
logosctl daemon status --json | jq -e '.daemon.status == "running"' > /dev/null
# Agent confirms daemon is up via exit code 0.

# Step 2: Check what modules are available
logosctl module ls --json
# Agent parses:
# [
#   {"name":"waku","version":"0.1.0","status":"not_loaded"},
#   {"name":"chat","version":"0.2.0","status":"not_loaded"},
#   {"name":"store","version":"0.3.0","status":"not_loaded"}
# ]
# Agent reads the array and identifies "chat" is available.

# Step 3: Load the chat module
logosctl module load chat
# Agent parses:
# {"status":"ok","module":"chat","version":"0.2.0","dependencies_loaded":["waku","store"]}
# Agent confirms status is "ok" and notes that waku and store were auto-loaded.

# Step 4: Discover what methods are available
logosctl module show chat --json
# Agent parses:
# {
#   "name": "chat",
#   "version": "0.2.0",
#   "status": "loaded",
#   "pid": 23457,
#   "uptime_seconds": 5,
#   "dependencies": ["waku", "store"],
#   "methods": [
#     {"name": "send_message", "signature": "send_message(QString)", "returnType": "QString", "isInvokable": true, "description": "Sends a chat message to the active channel.", "parameters": [{"name": "text", "type": "QString"}]},
#     {"name": "get_history", "signature": "get_history()", "returnType": "QJsonArray", "isInvokable": true, "description": "Returns the message history for the active channel.", "parameters": []},
#     {"name": "get_status", "signature": "get_status()", "returnType": "QString", "isInvokable": true, "parameters": []}
#   ],
#   "events": [
#     {"name": "message_received", "signature": "message_received(QString,QString)", "description": "Emitted when a new message arrives on the active channel.", "parameters": [{"name": "from", "type": "QString"}, {"name": "body", "type": "QString"}]}
#   ]
# }
# Agent now knows send_message takes a text param and returns a string, and —
# from each method's "description" — what it does, without any external docs.
# The "events" array tells it which events it can watch (and what they mean).

# Step 5: Call a method
logosctl call chat send_message "hello from agent"
# Agent parses:
# {"status":"ok","module":"chat","method":"send_message","result":"message sent (id: msg_9x8y7z)"}
# Agent confirms status is "ok".

# Step 6: Verify the message was stored
logosctl call chat get_history
# Agent parses:
# {"status":"ok","module":"chat","method":"get_history","result":[{"id":"msg_9x8y7z","from":"agent","text":"hello from agent","timestamp":"2026-03-23T14:30:01Z"}]}
# Agent verifies the message appears in history.

# Step 7: Check overall system health
logosctl daemon status --json | jq '.modules_summary'
# Agent parses:
# {"loaded": 3, "crashed": 0, "not_loaded": 0}
# All modules healthy. Agent can also check per-module resource usage via `logosctl stats`.
```

### Example: Agent Handles Errors

When an agent encounters an error, the structured output lets it self-correct:

```bash
# Agent tries to call a method on a module that isn't loaded
logosctl call delivery send_package "pkg_123"
# Exit code: 3
# {"status":"error","code":"MODULE_NOT_LOADED","message":"Module 'delivery' is not loaded. Load it with: logosctl module load delivery"}

# Agent reads the error code "MODULE_NOT_LOADED" and the recovery instruction.
# It follows the suggestion:
logosctl module load delivery
# {"status":"ok","module":"delivery","version":"0.1.0","dependencies_loaded":[]}

# Now retries the original call:
logosctl call delivery send_package "pkg_123"
# {"status":"ok","module":"delivery","method":"send_package","result":"package pkg_123 queued"}
```

### Example: Agent Monitors Events

An agent can watch for events to react to real-time activity:

```bash
# Start watching in background, capture output to a file
logosctl watch chat --event chat-message > /tmp/chat_events.log &
WATCH_PID=$!

# ... agent does other work ...

# Later, check what events arrived
cat /tmp/chat_events.log
# {"timestamp":"2026-03-23T14:30:01Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"hello"}}
# {"timestamp":"2026-03-23T14:30:05Z","module":"chat","event":"chat-message","data":{"from":"bob","text":"hi there"}}

# Agent can parse each line independently (NDJSON).
# Each line is valid JSON, so standard tools work:
# cat /tmp/chat_events.log | head -1 | jq '.data.from'
# → "alice"

# Cleanup
kill $WATCH_PID
```

### Why These Patterns Matter for Agents

| Pattern | Why it helps agents |
|---------|-------------------|
| JSON auto-detection (non-TTY) | Agent doesn't need to remember `--json` — it gets structured output automatically |
| Exit codes per error category | Agent can branch: `if exit_code == 2, start daemon; if exit_code == 3, load module` |
| Error messages with recovery commands | Agent can extract and execute the suggested fix directly |
| `status` as single dashboard | One command gives daemon health + all module states — no need to chain multiple commands |
| `module-info` with method signatures + descriptions | Agent discovers available operations and their intent — it reads each method's schema and `description` to construct calls without external docs |
| `module-info` with crash metadata | Agent can programmatically distinguish OOM (137/SIGKILL) from segfault (139/SIGSEGV) from clean error (non-zero) |
| `reload-module` on unloaded module | Falls back to load instead of erroring — reduces edge cases for agents that just want a module running |
| NDJSON streaming | Agent processes events line-by-line without buffering the full stream |
| No interactive prompts | Agent never hangs waiting for input it can't provide |
| Consistent JSON envelope (`status`, `code`) | Agent uses the same parsing logic for all commands |

---

## Sequence Flows

### Starting the Daemon and Loading Modules

```
1. START DAEMON
   logosctl daemon start -m /path/to/modules
   → Core initializes
   → Daemon mints "auto" token (local_only=true) for the local client
   → Scans /path/to/modules for available plugins
   → Writes ~/.logosctl/daemon/state.json (instance + resolved listeners)
   → Writes ~/.logosctl/daemon/tokens.json (hashed accepted-token list)
   → Emits ~/.logosctl/client/config.json + ~/.logosctl/client/auto.json
   → Runs event loop (foreground)

2. LOAD MODULES
   logosctl module load waku
   → Reads ~/.logosctl/client/config.json (dial spec + token_file)
   → Loads token from the file token_file points at
   → Connects to daemon's `core_service` via RPC with token
   → Daemon resolves dependencies for "waku"
   → Daemon loads dependencies first, then waku
   → Client prints result and exits

3. CALL METHODS
   logosctl call chat send_message "hello"
   → Reads dial spec + token from ~/.logosctl/client/
   → Connects to daemon
   → Invokes chat.send_message("hello") via RPC
   → Prints return value to stdout
   → Exits

4. WATCH EVENTS
   logosctl watch chat --event chat-message --json >> events.log &
   → Connects to daemon with token
   → Registers event listener for chat::chat-message
   → Streams NDJSON to stdout (redirected to events.log)
   → Runs until killed

5. STOP DAEMON
   logosctl daemon stop
   → Client sends shutdown RPC to core_service
   → Daemon schedules quit (with brief delay to send RPC response)
   → Daemon unloads all modules
   → Removes ~/.logosctl/daemon/state.json (tokens.json + config.json survive)
   → Exits

   Alternatively: Ctrl+C / kill <pid> / SIGTERM
   → Signal handler triggers QCoreApplication::quit()
   → Same cleanup as above
```

