#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# The Web container, end to end, through logoscore.
#
#   web-container-suite.sh <logoscore> <logoscore-webhost> <modules-dir>
#
# Everything here is driven from OUTSIDE, at the seam an operator uses: a
# daemon started with `--container web`, and the `load-module` / `call` /
# `watch` / `stats` client subcommands. Nothing reaches into the container, the
# transport or the token store -- what is asserted is what a caller can see.
#
# It is the twin of logos-test-modules' `--container inproc` run: same tool,
# same client commands, the other container. Both are nix checks, which is the
# only way "CI exercises both containers" means anything.
#
# Runs anywhere a Qt WebEngine process can start. `logoscore` itself links no
# browser; the page host is a separate binary, named by LOGOSCORE_WEBHOST.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

LOGOSCORE="${1:?usage: web-container-suite.sh <logoscore> <webhost> <modules-dir>}"
WEBHOST="${2:?}"
MODULES_DIR="${3:?}"

# Per-call ceiling. A page is a browser: a cold start is seconds, not
# milliseconds, and the first call to a module waits for its contract.
CALL_TIMEOUT="${TEST_TIMEOUT:-60}"

command -v jq >/dev/null 2>&1 || { echo "ERROR: jq is required." >&2; exit 1; }

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; }
# Compares one jq expression against one expected value, and prints what it saw
# when they differ -- a bare "FAIL add(1,2)" would send the reader to the log
# for the one thing the assertion already had in hand.
expect() {
  local what="$1" expected="$2" actual="$3"
  if [[ "$actual" == "$expected" ]]; then ok "$what"
  else bad "$what -- expected [$expected], got [$actual]"; fi
}

LOGOSCORE_CONFIG_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t 'logoscore-web-cfg')"
export LOGOSCORE_CONFIG_DIR
export LOGOSCORE_WEBHOST="$WEBHOST"
# The page host runs offscreen unless the environment already chose a platform.
# A check has no display; say so rather than relying on that default.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_FORCE_STDERR_LOGGING=1

DAEMON_LOG="$LOGOSCORE_CONFIG_DIR/daemon.log"
WATCH_LOG="$LOGOSCORE_CONFIG_DIR/watch.log"
DAEMON_PID=""
WATCH_PID=""

cleanup() {
  if [[ -n "$WATCH_PID" ]]; then
    kill "$WATCH_PID" 2>/dev/null
    wait "$WATCH_PID" 2>/dev/null
  fi
  if [[ -n "$DAEMON_PID" ]]; then
    "$LOGOSCORE" stop >/dev/null 2>&1
    kill "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

call() { timeout "$CALL_TIMEOUT" "$LOGOSCORE" "$@" 2>&1; }

# Re-asks a check function until it is satisfied, up to <tries> times half a
# second apart. The function prints the value it looked at and returns 0 when
# that value satisfies it; $POLLED keeps the last one.
#
# One helper rather than a loop per step because the alternative -- which this
# was -- writes the wait condition twice, once to break out of the loop and
# once to decide the assertion, and the two copies drift.
POLLED=""
poll() {
  local tries="$1" check="$2"
  for _ in $(seq 1 "$tries"); do
    POLLED="$("$check")" && return 0
    sleep 0.5
  done
  return 1
}

# ── the daemon ───────────────────────────────────────────────────────────────
echo "Web container suite"
echo "  logoscore:  $LOGOSCORE"
echo "  webhost:    $WEBHOST"
echo "  modules:    $MODULES_DIR"

"$LOGOSCORE" -D -m "$MODULES_DIR" --container web \
  --persistence-path "$LOGOSCORE_CONFIG_DIR/data" \
  >"$DAEMON_LOG" 2>&1 &
DAEMON_PID=$!

ready=0
for _ in $(seq 1 150); do
  if "$LOGOSCORE" status >/dev/null 2>&1; then ready=1; break; fi
  kill -0 "$DAEMON_PID" 2>/dev/null || break
  sleep 0.2
done
if [[ "$ready" -ne 1 ]]; then
  echo "ERROR: the daemon never became reachable. Log:" >&2
  cat "$DAEMON_LOG" >&2
  exit 1
fi

# 1. The policy is an assertion, and the daemon says which one it is holding.
#    `auto` would run exactly the same modules and prove nothing.
if grep -q "Container policy: web" "$DAEMON_LOG"; then
  ok "the daemon is holding the web container policy"
else
  bad "the daemon was not started with --container web"
fi

# 2. A page is a module: discovered from its manifest, loaded on demand.
expect "load-module js_counter" "ok" \
  "$(call load-module js_counter | jq -r '.status // "no-status"')"

# 3. THE CRITERION. add(1, 2) is 3, computed by JavaScript in a webview and
#    returned to a CLI over the web transport.
expect "js_counter.add(1, 2) == 3" "3" \
  "$(call call js_counter add 1 2 | jq -r '.result // "no-result"')"

# 4. One live module across calls, not a function evaluated fresh each time.
call call js_counter increment 7 >/dev/null
expect "the page keeps its state between calls" "7" \
  "$(call call js_counter value | jq -r '.result // "no-result"')"

# 5. A page's event reaches a native subscriber. The watcher is started first
#    and given a moment, because a Subscribe that arrives after the emit is a
#    subscription to the next one.
timeout "$CALL_TIMEOUT" "$LOGOSCORE" watch js_counter --event counted >"$WATCH_LOG" 2>&1 &
WATCH_PID=$!
sleep 3
call call js_counter increment 5 >/dev/null
# The payload of the last `counted` line the watcher has written, if any.
counted_payload() {
  local seen
  seen="$(jq -r 'select(.event == "counted") | .data.arg0' <"$WATCH_LOG" 2>/dev/null | tail -1)"
  printf '%s' "$seen"
  [[ -n "$seen" ]]
}
poll 30 counted_payload
expect "a native watcher receives the page's event" "12" "$POLLED"
kill "$WATCH_PID" 2>/dev/null; wait "$WATCH_PID" 2>/dev/null; WATCH_PID=""

# 6. The page calling OUT, as the module. The container grants nothing of its
#    own: this runs the requestModule handshake against the real
#    capability_module and presents what it minted.
listed="$(call call js_counter callNative modules_state list_modules \
          | jq -r '[.result.modules[]?.module] | sort | join(",")')"
case "$listed" in
  *js_counter*) ok "the page calls a native module through its own LogosAPI" ;;
  *) bad "the page's call to modules_state -- got [$listed]" ;;
esac

# 7. The explicit half of the same handshake: a credential capability_module
#    minted for a page. Asserted by SHAPE, since the value is a fresh uuid.
token="$(call call js_counter grant modules_state | jq -r '.result // ""')"
if [[ "$token" =~ ^[0-9a-f-]{36}$ ]]; then
  ok "capability_module mints a token for the page"
else
  bad "requestModule for the page -- got [$token]"
fi

# 8. And the reverse direction: a native module's event arriving INSIDE the
#    page. Loading js_other is the state change; the page's own JS handler is
#    what has to have run for heardEvents to be non-empty.
call call js_counter watchNative modules_state module_state_changed >/dev/null
expect "load-module js_other" "ok" \
  "$(call load-module js_other | jq -r '.status // "no-status"')"
# How many events the page's own JS handler has recorded, once it has any.
heard_event_count() {
  local n
  n="$(call call js_counter heardEvents | jq -r '.result | length')"
  printf '%s' "$n"
  [[ "$n" =~ ^[0-9]+$ && "$n" -gt 0 ]]
}
if poll 60 heard_event_count; then
  ok "the page heard a native module's event ($POLLED of them)"
else
  bad "the page heard nothing from modules_state -- heardEvents is [$POLLED]"
fi

# 9. A dead page. Killing the process that IS the module is the honest version
#    of a crash, and the three things that must hold are: the daemon lives, the
#    other page keeps answering, and the module is reported dead rather than
#    silently gone.
page_pid="$(call stats | jq -r '.[] | select(.name == "js_counter") | .pid')"
if [[ "$page_pid" =~ ^[0-9]+$ ]]; then
  kill -9 "$page_pid" 2>/dev/null
  # js_counter's reported status, once the daemon has stopped calling it loaded.
  counter_status() {
    local state
    state="$(call status | jq -r '.modules[] | select(.name == "js_counter") | .status')"
    printf '%s' "$state"
    [[ -n "$state" && "$state" != "loaded" ]]
  }
  if poll 60 counter_status; then
    ok "a killed page is reported as no longer running (status: $POLLED)"
  else
    bad "a killed page is still reported [$POLLED]"
  fi

  expect "the other page kept answering" "pong" \
    "$(call call js_other ping | jq -r '.result // "no-result"')"

  expect "the dead page loads again" "ok" \
    "$(call load-module js_counter | jq -r '.status // "no-status"')"
  expect "and answers again" "3" \
    "$(call call js_counter add 1 2 | jq -r '.result // "no-result"')"
else
  bad "no page-host pid for js_counter in stats -- got [$page_pid]"
fi

echo ""
echo "Web container: $PASS passed, $FAIL failed"
if [[ "$FAIL" -ne 0 ]]; then
  echo "--- daemon log ---"
  cat "$DAEMON_LOG"
  exit 1
fi
