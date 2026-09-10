# `logoscore`

The original headless CLI. Its commands, flags, config format and
`~/.logoscore` session directory are **unchanged** — everything here worked
the same before the repo started shipping a second binary, and still does.

`logoscore` will be removed once [`logosctl`](logosctl.md) has been validated
in real use. Until then it is the tool to use, and it is released separately.

Build instructions, flake outputs and test targets are in the
[main README](../README.md).

## Usage

`logoscore` runs as a **daemon** (long-running process) that you drive with **client commands** to load modules and call methods.

### Daemon Mode

Start a daemon, then use client commands to manage modules and call methods.

#### Starting the Daemon

```bash
# Start the daemon with module directories
logoscore -D -m ./modules
logoscore daemon --modules-dir ./modules

# Start in the background
logoscore -D -m ./modules > logs.txt &
```

The daemon writes a runtime-state file (`~/.logoscore/daemon/state.json`) on startup, after transports actually bind, and removes it on clean shutdown. It also auto-emits a local-client config under `~/.logoscore/client/` (`config.json` + `auto.json`) so client commands work out of the box from the same machine.

Layout:

```
~/.logoscore/
├── daemon/
│   ├── config.json        # operator preferences (written ONLY when --persist-config is passed)
│   ├── state.json         # live-instance resolved state (created at boot, removed at shutdown)
│   ├── tokens.json        # hashed-at-rest accepted-token list (survives restarts)
│   └── tokens/
│       └── <name>.json    # raw, operator-copyable per `issue-token <name>`. 0600 perms.
└── client/
    ├── config.json        # client-owned: dial spec + token_file (write only on --persist-config)
    └── auto.json          # raw token; daemon-emitted at boot for the local client
```

Each daemon-side file has one writer and a clear lifetime:
- **`config.json`** — operator-typed preferences (transport choices, modules dirs, SSL paths). Values reflect intent: `port: 0` stays `0` (auto-pick a free port). Only written when the operator explicitly passes `--persist-config`.
- **`state.json`** — what this specific daemon process resolved (instance_id, pid, started_at, *actually-bound* port). Created at boot, deleted at shutdown.
- **`tokens.json`** — the hashed-at-rest accepted-token list. Independent of the running daemon's lifetime.

The daemon never reads `client/`; the client never reads `daemon/config.json` or `daemon/tokens.json`. (`status` consults `daemon/state.json` for a fast same-host liveness check via `kill(pid, 0)`, but never opens daemon-only secrets.) For remote clients on a different host, copy a single `daemon/tokens/<name>.json` file to the target host's `client/` dir and reference it via `token_file` in `client/config.json`.

#### `--persist-config`

CLI flags affect this run only by default. To bake them into the next launch — daemon or client side — pass `--persist-config`:

```bash
# Run the daemon with TCP listeners; this run only.
logoscore -D --module-transport core_service=tcp,port=0

# Same flags + persist them. config.json is written; next no-flag launch
# reproduces the TCP behavior. `port: 0` stays `0` in config.json (intent
# preserved); the actual bound port lives in state.json.
logoscore -D --module-transport core_service=tcp,port=0 --persist-config
```

Boot precedence is `defaults < config.json < CLI args`, with per-flag override detection: a CLI flag overrides only its own field, everything else falls through. Without `--persist-config`, no file is written.

#### Client Commands

All client commands connect to the running daemon. When stdout is not a TTY (piped or redirected), output is JSON by default. Use `--json` / `-j` to force JSON in a terminal.

```bash
# Check daemon health
logoscore status
logoscore status --json

# Load a module (dependencies resolved automatically)
logoscore load-module waku
logoscore load-module chat --json

# Unload a module
logoscore unload-module waku

# Reload a module (unload + load; falls back to load if not loaded)
logoscore reload-module chat

# List all discovered modules
logoscore list-modules
logoscore list-modules --loaded    # only loaded modules

# Get detailed module info (methods + events + descriptions, dependencies, crash details)
logoscore module-info chat
logoscore info chat                # alias

# Call a method on a loaded module
logoscore call chat send_message "hello world"
logoscore call storage load_config @config.json   # @file reads from file

# Pass a list / map / nested value with the json: prefix (see "Argument typing")
logoscore call greeter greetMany 'json:["ada","alan"]'
logoscore call config    apply    'json:{"retries":3,"debug":true}'
logoscore call greeter greet     'str:json:literal'   # str: forces a literal string

# Alternative verbose call syntax
logoscore module chat method send_message "hello"

# Watch events from a module (streams until Ctrl+C)
logoscore watch chat --event chat-message
logoscore watch chat --json        # NDJSON output

# Show resource usage for loaded modules
logoscore stats
```

#### Argument typing

Each positional argument to `call` is turned into a JSON value using the first
rule that matches, so scalars stay ergonomic while lists, maps, and literal
strings are all expressible:

| Argument form | Becomes | Example |
|---------------|---------|---------|
| `json:<value>` | the value parsed as JSON (list / map / number / any nested value) | `json:[1,2,3]`, `json:{"k":"v"}` |
| `json:@<file>` | the file's contents parsed as JSON | `json:@payload.json` |
| `str:<text>` | `<text>` verbatim as a string — no parsing, no coercion | `str:json:x` → `"json:x"`, `str:42` → `"42"` |
| `@<file>` | the file's raw contents as a string | `@config.json` |
| `true` / `false` | a boolean | `true` |
| a whole number | an integer | `42` |
| a decimal number | a double | `3.14` |
| anything else | a string | `hello` |

`json:` and `str:` are the two explicit escapes, mirroring the convention used
by `jq` (`--arg` / `--argjson`) and HTTPie (`=` / `:=`): the default path never
guesses a container, `json:` opts into parsing, and `str:` forces a literal
string for any value the default rules would otherwise reinterpret (a
number-like string, or one that itself starts with `json:` / `str:` / `@`).

**Binary (`bstr`) arguments.** JSON has no native byte type, so bytes use the
canonical tagged encoding — a JSON object `{"_bytes": "<base64url, unpadded>"}`.
Pass it like any other JSON value with `json:`:

```bash
# base64url("hello") == "aGVsbG8"
logoscore call blobstore put 'json:{"_bytes":"aGVsbG8"}'
logoscore call blobstore put 'json:@blob.json'   # {"_bytes":"..."} from a file
```

#### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | General error / daemon not running (for `status`) |
| `2` | No daemon running |
| `3` | Module error (not found, load/unload failed) |
| `4` | Method error (not found, call failed, timeout) |

#### Authentication

For local same-host use, token management is automatic. At boot the daemon
auto-issues a token named `auto` (with `local_only=true`, so it can't be used
over TCP), writes the hash into `daemon/tokens.json`, and emits the raw value
into `client/auto.json`. On the *first* boot into an empty config dir it
also writes a default `client/config.json` so local client commands work
out of the box; subsequent boots leave an existing `client/config.json`
alone (so an operator-written remote-client config isn't clobbered).

For remote or programmatic access:

```bash
# Via environment variable
LOGOSCORE_TOKEN=<token> logoscore list-modules --json
```

Token resolution order: `LOGOSCORE_TOKEN` env var → `<configDir>/client/<token_file>` (the path is whatever `client/config.json` says — defaults to `auto.json`).

##### Named client tokens

For multi-client setups (a daemon serving several remote clients, CI rotating
credentials, etc.), issue named tokens. Each entry persists as a `{name, hash,
issued_at, expires_at, local_only}` row in `daemon/tokens.json["tokens"]`; the
raw value is written to `daemon/tokens/<name>.json` (mode 0600) at issue time
so the operator can hand it off:

```bash
# Issue a token for "alice"
logoscore issue-token --name alice
logoscore issue-token --name alice --replace            # rotate
logoscore issue-token --name ci    --expires 30d        # expires after 30 days
logoscore issue-token --name probe --local-only         # only valid over LocalSocket

# List all issued tokens (names, issued_at, expires_at, local_only flag)
logoscore list-tokens

# Revoke by name — next request with that token fails auth
logoscore revoke-token alice
```

The raw value is written to `daemon/tokens/<name>.json` so the operator can hand
it off to a client host.

Operator-issued tokens authorize immediately — the daemon validates every RPC
against `daemon/tokens.json` on the call path (a fresh hash lookup per call), so
`issue-token` grants access, `revoke-token` removes it, and `--expires` and
`--local-only` are enforced without a restart. A `--local-only` token is
accepted only over the local socket; presented over TCP/TLS it is rejected, so a
leaked local token can't be replayed across the network. (The raw
`daemon/tokens/<name>.json` file may be deleted after handoff — validation uses
the hash in `daemon/tokens.json`.)

##### Plaintext-TCP guard

Plaintext `tcp` on a non-loopback host puts tokens on the wire in cleartext.
The daemon refuses to bind such a listener unless `--insecure-tcp` is
explicitly passed:

```bash
# Refused — would expose tokens.
logoscore -D --module-transport core_service=tcp,host=0.0.0.0,port=6000

# Use TLS instead.
logoscore -D \
    --module-transport core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/path/cert.pem,key=/path/key.pem

# Or, for trusted-network test setups only:
logoscore -D --module-transport core_service=tcp,host=0.0.0.0,port=6000 --insecure-tcp
```

#### Parallel Daemons (`--config-dir`)

`--config-dir <path>` overrides the default `~/.logoscore` location for the entire `daemon/` and `client/` subtree (and the module persistence tree). This lets multiple `logoscore` daemons run side-by-side against isolated state. Client commands must be invoked with the same `--config-dir` as the daemon they target.

```bash
# Two parallel daemons with isolated config/state
logoscore --config-dir /tmp/ls-a -D -m ./modules &
logoscore --config-dir /tmp/ls-b -D -m ./modules &

# Target each daemon explicitly
logoscore --config-dir /tmp/ls-a status
logoscore --config-dir /tmp/ls-b load-module waku

# Stop cleanly
logoscore --config-dir /tmp/ls-a stop
logoscore --config-dir /tmp/ls-b stop
```

Resolution order: `--config-dir` → `LOGOSCORE_CONFIG_DIR` env var → `~/.logoscore`. The flag also mirrors into `LOGOSCORE_CONFIG_DIR` so child processes inherit it.

#### Running as a system service, shared with an OS group (`--access-group`)

A common deployment runs the daemon as a dedicated service user (e.g. `logos`,
with `--config-dir /var/lib/logos-node/.logoscore`) and lets a person's own OS
account drive it. By default that fails: the daemon's local sockets are bound
owner-only (connecting to a unix socket needs *write* permission on the socket
file), and the client config/token it writes are `0600`.

`--access-group <group>` shares the daemon with an OS group:

- the module sockets are chgrp'd to the group and set to `0660`, so a member can
  connect;
- `client/config.json` and `client/auto.json` become `0640` + group-owned, and
  the config dir is made group-traversable, so a member can read the dial spec
  and the shared token (`daemon/` stays owner-only — private state is not
  shared). This is the same trust model as `docker.sock`: **group membership
  grants access.**

```bash
# As root / the service manager: create the group and add the human user.
sudo groupadd --system logos
sudo usermod -aG logos alice            # alice must re-login for this to apply

# Daemon, running as the service user `logos`:
logoscore -D -m /opt/logos/modules \
  --config-dir /var/lib/logos-node/.logoscore \
  --access-group logos

# As alice (a member of group `logos`), pointing at the service's config dir:
export LOGOSCORE_CONFIG_DIR=/var/lib/logos-node/.logoscore
logoscore status
logoscore list-modules
logoscore call my_module.some_method arg
```

The client config is regenerated on every boot, so `alice` never has to re-copy
it after a restart — the instance id changes, but the group-readable
`client/config.json` always reflects the running daemon.

> **systemd note:** do **not** set `PrivateTmp=yes` on the unit. It gives the
> service a private `/tmp` namespace, so the `/tmp/logos_*` sockets are
> *invisible* (not merely unreadable) to a client in another namespace and no
> permission change can bridge that. If you need `/tmp` isolation, that is a
> follow-up (relocating the sockets to a shared runtime dir).

#### Transports

> ⚠️ **Remote operation is very WIP and subject to change.** Everything in
> this section and the two that follow (network transports, the
> `client/config.json` dial spec, the remote client ↔ daemon walkthrough)
> is under active development. Flags, the config-file schema, and behavior
> may change without notice between releases. Local same-host use is the
> stable path; treat remote setups as experimental for now.

By default the daemon binds each well-known module (`core_service`,
`capability_module`) to a local Unix socket only — clients must run on the
same host. To reach the daemon from another machine, a container, or
across NAT, configure a network listener per module via `--module-transport`:

```
--module-transport NAME=PROTOCOL[,k=v[,k=v...]]
```

`NAME` is any module the daemon will load; `PROTOCOL` is `local`, `tcp`,
or `tcp_ssl`. The optional `k=v` pairs configure the protocol: `host`,
`port`, `codec` (`json` default | `cbor`), and
`ca` / `cert` / `key` / `verify_peer` for `tcp_ssl`. The flag is
repeatable — each appearance adds one more listener to the named module.

**Local is always present.** Every module the operator configures (and
the two well-known ones — `core_service` and `capability_module`)
automatically gets a `local` listener prepended to whatever the
operator named. The TCP / TCP+SSL flags add *additional*
outside-facing listeners; they don't replace the same-host one.

This means the examples below — and any `--module-transport NAME=tcp,...`
invocation generally — don't need a separate `--module-transport
NAME=local` line. The same-host LocalSocket listener is bound for free,
which lets every **intra-daemon** code path (cross-module outbound
`getClient(name)` calls, the parent's `notifyCapabilityModule`
handshake) keep working over LocalSocket while remote clients use the
operator-configured TCP endpoint.

> **Remote clients need `capability_module` exposed too — not just
> `core_service`.** A client command doesn't talk only to `core_service`.
> Before its first RPC, the client's own `LogosAPIClient` performs a
> `requestModule` handshake against **`capability_module`** to resolve the
> endpoint. On the same host that handshake rides the free LocalSocket
> listener, so a local client only ever needs `core_service`. A client on
> *another* host has no LocalSocket to the daemon — its `requestModule`
> call has to reach `capability_module` over the network. So a remote
> daemon must add a TCP (or `tcp_ssl`) listener to **both** well-known
> modules:
>
> ```bash
> logoscore -D -m ./modules \
>     --module-transport core_service=tcp,host=0.0.0.0,port=8645 \
>     --module-transport capability_module=tcp,host=0.0.0.0,port=8646 \
>     --insecure-tcp
> ```
>
> Expose only `core_service` and client commands hang or fail at connect
> time, because the `capability_module` handshake never completes. This is
> the single most common remote-setup mistake.

```bash
# TCP — plaintext, good for localhost or trusted networks. Local
# listeners are added automatically; just name the TCP one.
logoscore -D -m ./modules \
    --module-transport core_service=tcp,host=127.0.0.1,port=6000 \
    --module-transport capability_module=tcp,host=127.0.0.1,port=6001

# TCP + TLS — wire-encrypted; cert + key required, CA optional. Local
# listeners are still bound implicitly for same-host clients.
logoscore -D -m ./modules \
    --module-transport "core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem,ca=/etc/logoscore/ca.pem" \
    --module-transport "capability_module=tcp_ssl,host=0.0.0.0,port=6444,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem,ca=/etc/logoscore/ca.pem"

# Defaults: omit --module-transport entirely and the well-known modules
# get a single `local` listener each. Most local-development setups just
# want this.
logoscore -D -m ./modules

# Per-module: applies to user modules too. The operator's TCP listener
# is the additional surface; LocalSocket is always there for in-process
# / on-host callers.
logoscore -D -m ./modules \
    --module-transport my_module=tcp,host=127.0.0.1,port=6010
```

##### Client-side dial spec

The client never reads daemon-only files (`daemon/config.json`,
`daemon/tokens.json`) — it dials whatever `<configDir>/client/config.json`
says. The daemon auto-emits one for the local same-host case at boot
(LocalSocket pointing at the daemon's freshly-issued `auto.json` token).
For remote clients (docker `-p` port-forwarding, NAT, SSH tunnels) you
write `client/config.json` yourself.

**`client/config.json` schema** (`version` must be `2`):

```jsonc
{
    "version": 2,
    "token_file": "bob.json",      // filename inside the SAME client/ dir
    "instance_id": "a3f1c8d20b4e",   // OPTIONAL — only the local-socket dial
                                     // path needs it; omit for remote TCP
    "daemon": {
        "core_service": {
            "transport": "tcp",      // "local" | "tcp" | "tcp_ssl"
            "host":      "192.168.1.20",
            "port":      8645,
            "codec":     "json"      // OPTIONAL — "json" (default) | "cbor"
        },
        "capability_module": {       // REQUIRED for remote clients (see above)
            "transport": "tcp",
            "host":      "192.168.1.20",
            "port":      8646
        }
    }
}
```

Notes on the schema:
- The per-module key is `"transport"`, **not** `"protocol"`. The reader
  uses a strict allowlist: a typo (e.g. `"tcp_ssll"`) fails the whole parse
  with a clear error rather than silently dialing the wrong endpoint.
- `daemon.core_service` is mandatory; `daemon.capability_module` is required
  in practice for any remote (non-LocalSocket) client — see the callout in
  **Transports** above.
- The two entries can point at **different ports** — they're independent
  listeners on the daemon (e.g. `8645` and `8646` above).
- For `tcp_ssl`, add `"ca": "/path/ca.pem"` and `"verify_peer": true|false`.
- `host`/`port` are the **dial** address. Behind docker `-p`, NAT, or an SSH
  tunnel this is the reachable address, which may differ from the `0.0.0.0`
  the daemon bound.

**Where the file lives — `--config-dir`.** `client/config.json` and the
token file both live in a `client/` subdirectory. `--config-dir` points at
the directory that *contains* `client/`, not at `client/` itself:

```
my-client-dir/            ← pass this to --config-dir
└── client/
    ├── config.json       ← the dial spec above
    └── bob.json        ← the raw token file referenced by token_file
```

```bash
logoscore --config-dir ./my-client-dir status
logoscore --config-dir ./my-client-dir list-modules
```

(When unset, `--config-dir` defaults to `~/.logoscore`, so on the daemon's
own host the auto-emitted `~/.logoscore/client/` tree is used with no flag.)

##### End-to-end: remote client ↔ daemon

A complete walkthrough for a client on one host talking to a daemon on
another (`192.168.1.20`). Mirrors a real plaintext-TCP setup on a trusted
LAN; for anything crossing an untrusted network use `tcp_ssl` (final step).

**1. On the daemon host** — expose both well-known modules over TCP and
mint a token for this client:

```bash
# Bind core_service + capability_module on all interfaces. --insecure-tcp
# is required because plaintext tcp on a non-loopback host puts the token
# on the wire in cleartext (use tcp_ssl to avoid the flag — see below).
logoscore daemon --modules-dir /home/me/logos/modules/ \
    --module-transport core_service=tcp,host=0.0.0.0,port=8645 \
    --module-transport capability_module=tcp,host=0.0.0.0,port=8646 \
    --insecure-tcp &

# Mint a named token for the remote client. Writes the raw value to
# ~/.logoscore/daemon/tokens/bob.json (mode 0600).
logoscore issue-token --name bob
```

**2. Move the token to the client host.** Copy the raw token file across
(scp / ansible / your secret store) into the client's `client/` dir:

```bash
# Run on, or targeting, the client host:
mkdir -p ./my-client-dir/client
scp daemon-host:~/.logoscore/daemon/tokens/bob.json ./my-client-dir/client/
```

After copying, the daemon-side `daemon/tokens/bob.json` may be deleted —
the daemon validates against the stored hash, not the raw file.

**3. On the client host** — write `./my-client-dir/client/config.json`
pointing at the daemon's IP and the two ports, with `token_file` naming the
file you just copied:

```json
{
    "version": 2,
    "token_file": "bob.json",
    "daemon": {
        "core_service":      { "transport": "tcp", "host": "192.168.1.20", "port": 8645 },
        "capability_module": { "transport": "tcp", "host": "192.168.1.20", "port": 8646 }
    }
}
```

**4. Run client commands** with `--config-dir` pointing at the directory
that contains `client/`:

```bash
logoscore --config-dir ./my-client-dir status
logoscore --config-dir ./my-client-dir list-modules
logoscore --config-dir ./my-client-dir load-module accounts_module
logoscore --config-dir ./my-client-dir module-info accounts_module
logoscore --config-dir ./my-client-dir call accounts_module createRandomMnemonicWithDefaultLength
```

**TLS variant.** To drop `--insecure-tcp` and encrypt the wire, bind
`tcp_ssl` on the daemon and point the client at the CA:

```bash
# Daemon
logoscore daemon -m /home/me/logos/modules/ \
    --module-transport "core_service=tcp_ssl,host=0.0.0.0,port=8645,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem" \
    --module-transport "capability_module=tcp_ssl,host=0.0.0.0,port=8646,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem"
```

```json
// client/config.json — transport becomes tcp_ssl + ca/verify_peer
{
    "version": 2,
    "token_file": "bob.json",
    "daemon": {
        "core_service":      { "transport": "tcp_ssl", "host": "192.168.1.20", "port": 8645, "ca": "/etc/logoscore/ca.pem", "verify_peer": true },
        "capability_module": { "transport": "tcp_ssl", "host": "192.168.1.20", "port": 8646, "ca": "/etc/logoscore/ca.pem", "verify_peer": true }
    }
}
```

#### Agent / Script Example

```bash
# Start daemon
logoscore -D -m ./modules &
sleep 2

# Preflight: verify daemon is running
logoscore status --json | jq -e '.daemon.status == "running"' > /dev/null

# Load modules
logoscore load-module chat --json

# Discover available methods (with their documentation)
logoscore module-info chat --json | jq '.methods[] | {name, description}'

# Discover the events a module emits (with their documentation)
logoscore module-info chat --json | jq '.events[] | {name, description}'

# Call a method
logoscore call chat send_message "hello from script" --json

# Auto-reload any crashed modules
logoscore list-modules --json | jq -r '.[] | select(.status == "crashed") | .name' | while read mod; do
  logoscore reload-module "$mod" --json
done

# Stream events to a log file
logoscore watch chat --event chat-message --json >> events.log &
```

#### Events Example

Modules can emit events that you can listen to in real time. Use `watch` to subscribe and `call` to trigger:

```bash
# Start daemon with a modules directory
logoscore -D -m ./modules_dir &
sleep 2

# Load the module
logoscore load-module test_basic_module

# Start watching for events in the background, writing to a file
logoscore watch test_basic_module --event testEvent > events.txt &
WATCH_PID=$!

# Trigger the event from another call
logoscore call test_basic_module emitTestEvent "hello world"

# Check the captured event
cat events.txt

# Clean up
kill $WATCH_PID
logoscore stop
```

### Quick start: load modules and call methods

The daemon starts **clean** (it scans the module directories but loads nothing
on its own). Load modules with `load-module` — transitive dependencies are
resolved automatically — then call methods:

```bash
# Start a clean daemon scanning ./modules
logoscore -D -m ./modules

# Load modules (deps resolved automatically)
logoscore load-module waku
logoscore load-module chat

# Call methods (positional args — see "Argument typing" below)
logoscore call chat send_message hello
logoscore call storage init config 42 true
logoscore call storage loadConfig @config.json           # @file → raw file contents
logoscore call storage setTags 'json:["a","b"]'          # json: → parsed list/map/value
logoscore call storage setLabel 'str:42'                 # str: → literal string "42"

# Multiple module directories + a custom persistence path
logoscore -D -m ./core-modules -m ./extra-modules --persistence-path /tmp/test-data

# Stop the daemon when done
logoscore stop
```

Daemon startup options:

```
  -D                             Start the daemon
  -m, --modules-dir <path>       Directory to scan for modules (repeatable)
      --persistence-path <path>  Base directory for module instance persistence
                                 (default: ~/.logoscore/data)
      --access-policy <arg>      Inter-module access policy. `enforce` turns on
                                 deny-by-default; also accepts a path to a JSON
                                 file, or inline JSON (mode + per-target caller
                                 allowlists). Default: none (no enforcement).
                                 See "Access policy" below.
```

#### `--container`: which container a module must run in

An **assertion**, not a switch. Which container runs a module is decided by its
**artifact** — a Bare module image runs in-process in the Native container, a Qt
plugin runs in a subprocess host, a `web` variant (a package whose `main` is an
`.html` document) runs in a webview in the Web container — and no flag turns one
into another. `--container inproc` therefore means "every module here must be a
Bare module", and a Qt plugin under it is refused rather than quietly
subprocessed. That is what lets a CI job assert which container actually ran the
thing.

```
      --container <auto|inproc|subprocess|web>
```

`auto` (the default) asserts nothing.

##### Running a `web` module

`--container web` additionally needs this process to have somewhere to run a
page. It does not: `logoscore` links no browser, deliberately — a headless CLI
should not carry Chromium to run modules that are not pages. The page runs in a
**separate `logoscore-webhost` process**, which the daemon starts per module and
speaks the web transport to over a loopback socket. That separation is also what
makes a dead page survivable: the host sees the socket end, marks the module
unloaded and keeps running.

```bash
# The page host is a nix output of its own.
nix build github:logos-co/logos-logoscore-cli#webhost

# A modules directory with one `web` module in it, for trying this out.
nix build github:logos-co/logos-logoscore-cli#web-fixture -o fixture

LOGOSCORE_WEBHOST=./result/bin/logoscore-webhost \
  logoscore -D --modules-dir ./fixture/modules --container web

logoscore load-module js_counter
logoscore call js_counter add 1 2          # -> {"result":3,...}
logoscore watch js_counter --event counted &
logoscore call js_counter increment 7      # -> the subscriber sees counted(7)
logoscore stats                            # -> js_counter, with the page host's pid
```

`LOGOSCORE_WEBHOST` names the binary. Without it the daemon looks for
`logoscore-webhost` beside itself, which is where a build with
`-DLOGOSCORE_WITH_WEBENGINE=ON` puts it; with neither, a `web` module reports
the missing bridge by name at load and nothing else changes.

The page host runs offscreen unless `QT_QPA_PLATFORM` is already set, so
exporting `QT_QPA_PLATFORM=cocoa` (or leaving it at your desktop's default) is
how you watch a page you are debugging. Its JavaScript console goes to the
daemon's stderr.

**What a module page has to do.** Nothing host-specific but one line: the host
hands it `window.logosChannelReady`, a Promise of a
[logos-js-sdk](https://github.com/logos-co/logos-js-sdk) channel, and the
browser build of the SDK does the rest.

```html
<script src="logos-web.js"></script>
<script>
(async function () {
  const channel = await window.logosChannelReady;
  const provider = new LogosWeb.WebProvider('js_counter');
  provider.register({
    handlers: { add: (a, b) => Number(a) + Number(b) },
    events: ['counted'],
    // The credential the core minted for this module. Saving it shuts the
    // door: from here on a call must carry it.
    onToken: (moduleName, token) => provider.saveToken(moduleName, token),
  });
  provider.attach(channel);
})();
</script>
```

#### Access policy

**Default: off.** Without `--access-policy`, any loaded module may call any
other — unchanged from every earlier release.

##### Turning deny-by-default on

```bash
logoscore -D -m ./modules --access-policy enforce
```

That arms **deny-by-default**: a module may only call the modules it declared
as dependencies in its `metadata.json`. The runtime derives each target's
allowed callers from the live dependency graph (its loaded dependents, plus the
trusted `core` / `core_service`) and registers them with `capability_module`,
which then refuses to mint a token for anyone else — so a call from an
undeclared caller can never proceed.

A refusal is logged by `capability_module` with **both** module names, so a
denial never presents as a mysteriously empty result:

```
[capability_module] access policy denies 'test_basic_module' -> 'test_extlib_module'
```

The daemon also states which side it landed on at startup, so a policy that
failed to arm is visible rather than silently permissive:

```
Inter-module access enforcement is ON (mode=enforce): deny-by-default — ...
Inter-module access enforcement is OFF (no access policy set): ...
```

> **Before you flip it on:** modules that call targets they never declared will
> start being refused. Out-of-process `ui_qml` plugins are the known case — they
> are not tracked as dependents in the core registry, so they need an explicit
> `restrictions` entry (below). Check a deployment against the log first.

##### Full policy documents

`--access-policy` also accepts a policy document, which declares per target
module which caller modules may invoke it. The argument is resolved as the
literal **`enforce`**, else **a path to a JSON file** when it doesn't begin with
`{`, else **inline JSON**. The resolved document is validated as parseable JSON
before the daemon boots; a bad path or malformed JSON aborts startup.

```json
{
  "version": 1,
  "mode": "enforce",
  "restrictions": {
    "package_manager":    { "allowedCallers": ["package_manager_ui"] },
    "package_downloader": { "allowedCallers": ["package_manager_ui"] }
  }
}
```

`mode` is the switch: only `"enforce"` activates gating, and `--access-policy
enforce` is shorthand for exactly `{"version":1,"mode":"enforce","restrictions":{}}`.
An entry in `restrictions` **replaces** the derived allow-list for that target
verbatim — that is the escape hatch for a caller that legitimately cannot
declare its target. `capability_module`, `core` and `core_service` are never
restricted as targets.

```bash
# From a file
logoscore -D -m ./modules --access-policy ./policy.json

# Inline
logoscore -D -m ./modules \
  --access-policy '{"version":1,"mode":"enforce","restrictions":{"package_manager":{"allowedCallers":["package_manager_ui"]}}}'
```

The policy is handed to the runtime (via `logos_core_set_access_policy`)
before any module is loaded, and is persisted with `--persist-config`
like the other daemon flags.

> **Note:** the legacy inline mode (`-c "module.method(args)"` / `--quit-on-finish`,
> which ran calls in a single short-lived process) has been removed. Use a daemon
> plus `logoscore call ...` as shown above.

