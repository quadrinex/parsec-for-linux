#!/usr/bin/env bash
# Virtual display keeper: with PLH_VIRTUAL_DISPLAY_AUTO on and no real output at all (screens asleep and
# unplugged by the monitor while the session is locked), the bridge holds a virtual output itself so Wine has
# a monitor. Private bridge (own runtime dir); "no real output" is simulated with PLH_FAKE_NO_OUTPUTS=1.
# Creates a real KWin virtual output for ~10 s (intrusive). Not covered: the real zero-output state.
. "$(dirname "$0")/lib/common.sh"
need_intrusive
need_cmd kscreen-doctor
BR="$REPO/build/bridge/plh-bridge"
[ -x "$BR" ] || skip "build/bridge/plh-bridge missing (make -C src/bridge)"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no Wayland session"
vc() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -c 'Output: .*Virtual-plh'; }
exec 9>/tmp/plh-vout.lock
flock -w 180 9 || skip "/tmp/plh-vout.lock busy for 180 s"
[ "$(vc)" = 0 ] || skip "a virtual output already exists (a client may be using it)"
# The bridge's socket path must stay short (sun_path): not under $TMPD.
mkdir -p /tmp/plh-keeper
RD=$(mktemp -d /tmp/plh-keeper/t43.XXXX); chmod 700 "$RD"
BP=
run() { ( exec env PLH_BRIDGE_DIR="$RD" PLH_INPUT_BACKEND=none "$@" "$BR" --runtime-dir "$RD" >>"$RD/log" 2>&1 ) & BP=$!; }
stop() { kill "$BP" 2>/dev/null; wait "$BP" 2>/dev/null; rm -f "$RD/bridge.sock" "$RD/port"; sleep 1; }
trap 'kill -KILL $BP 2>/dev/null; rm -rf "$RD"' EXIT

run PLH_VIRTUAL_DISPLAY_AUTO=1
sleep 10
kill -0 "$BP" 2>/dev/null || fail "private bridge exited: $(tail -2 "$RD/log")"
[ "$(vc)" = 0 ] || fail "real outputs present, but the keeper created a virtual output"
stop

run PLH_VIRTUAL_DISPLAY_AUTO=0 PLH_FAKE_NO_OUTPUTS=1
sleep 10
[ "$(vc)" = 0 ] || fail "PLH_VIRTUAL_DISPLAY_AUTO=0, but the keeper created a virtual output"
stop

run PLH_VIRTUAL_DISPLAY_AUTO=1 PLH_FAKE_NO_OUTPUTS=1
for i in $(seq 20); do sleep 1; [ "$(vc)" = 1 ] && break; done
[ "$(vc)" = 1 ] || fail "no real output (simulated), but no virtual output after 20 s: $(grep -E 'keeper|vout' "$RD/log" | tail -2)"
info "virtual output created after ${i} s"
sleep 7
[ "$(vc)" = 1 ] || fail "the keeper did not hold the virtual output"
grep -q 'keeper: no real output, holding' "$RD/log" || fail "keeper log line missing"
stop
for i in $(seq 10); do [ "$(vc)" = 0 ] && break; sleep 0.5; done
[ "$(vc)" = 0 ] || fail "virtual output still present after the bridge exited"
pass "keeper: idle with real outputs or AUTO off; holds a virtual output with no real output; removed on exit"
