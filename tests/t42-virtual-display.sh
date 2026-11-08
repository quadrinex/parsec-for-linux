#!/usr/bin/env bash
# Virtual display (D-020, docs/plan-virtual-display.md). Cases that need a component not delivered yet SKIP
# with the reason; they are listed at the end. Native cases create a real KWin virtual output for ~2 s each
# (visible in kscreen-doctor/xrandr only; no input, no host restart: outputs live < 3 s so layout-watch,
# which polls every 3 s and re-checks after 5 s of stability, cancels).
. "$(dirname "$0")/lib/common.sh"
need_cmd kscreen-doctor xrandr timeout
VP="$REPO/build/bridge/vout-probe"
[ -x "$VP" ] || skip "build/bridge/vout-probe missing (make -C src/bridge)"
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no Wayland session"
HOST="$REPO/bin/parsec-linux-host"
D="$TMPD/t42"; rm -rf "$D"; mkdir -p "$D"
SKIPS=()
note_skip() { SKIPS+=("$1"); info "SKIP case: $1"; }

vcount_kd() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -c 'Output: .*Virtual-plh'; }
vcount_xr() { xrandr --listmonitors 2>/dev/null | grep -c 'Virtual-plh'; }
wait_gone() { local i; for i in $(seq 1 40); do [ "$(vcount_kd)$(vcount_xr)" = 00 ] && return 0; sleep 0.25; done; return 1; }
wait_ready() { local i; for i in $(seq 1 40); do grep -q '^READY' "$1" 2>/dev/null && return 0; sleep 0.25; done; return 1; }

# Safety: never run while a client is connected (a new output would disturb the session) or when
# a virtual output is already present (not ours).
if [ -f "$HOME/.parsec/log.txt" ] || true; then :; fi
[ "$(vcount_kd)" = 0 ] || skip "a Virtual-plh output already exists (not touching it)"
# A client connected = last connect/disconnect line in the Parsec log is "connected." and parsecd runs.
if [ -x "$HOST" ] && pgrep -x parsecd >/dev/null 2>&1; then
  plog=$(ls -t "$REPO"/runtime/traces/parsec*.log "$PREFIX_DIR"/drive_c/users/*/AppData/Roaming/Parsec/log.txt 2>/dev/null | head -1)
  if [ -n "$plog" ] && tr -d '\r' <"$plog" | grep -aE ' (dis)?connected\.$' | tail -1 | grep -q ' connected\.$'; then
    skip "a Parsec client appears to be connected (not changing the layout)"
  fi
fi

# --- 1a. create -> appears in kscreen-doctor AND xrandr; destroy (clean close) -> gone
"$VP" -w 1920 -h 1080 -t 2 -n plh-virtual >"$D/a.out" 2>&1 &
pa=$!
wait_ready "$D/a.out" || { kill "$pa" 2>/dev/null; fail "1a: vout-probe never reached READY: $(tail -3 "$D/a.out" | tr '\n' ' ')"; }
[ "$(vcount_kd)" = 1 ] || { wait "$pa"; fail "1a: output not listed exactly once by kscreen-doctor (got $(vcount_kd))"; }
[ "$(vcount_xr)" = 1 ] || { wait "$pa"; fail "1a: output not listed exactly once by xrandr (got $(vcount_xr))"; }
# Placement: right of the rightmost screen, never overlapping a real one.
geom=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/{v=/Virtual-plh/} v&&/Geometry/{print $2}')
rx=$(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Output:/{v=/Virtual-plh/} !v&&/Geometry/{split($2,a,","); split($3,b,"x"); e=a[1]+b[1]; if(e>m)m=e} END{print m+0}')
vx=${geom%%,*}
[ -n "$geom" ] && [ "$vx" -ge "$rx" ] || { wait "$pa"; fail "1a: virtual output at '$geom' overlaps or is left of the real desktop (rightmost real edge $rx)"; }
wait "$pa"
wait_gone || fail "1a: output still present 10 s after a clean close"
info "1a ok: appears once in kscreen-doctor and xrandr at x=$vx (real edge $rx), gone after close"

# --- 1b. SIGKILL of the owner -> KWin drops it (crash safety)
"$VP" -k -n plh-virtual >"$D/b.out" 2>&1 &
pb=$!
wait_ready "$D/b.out" || { kill -KILL "$pb" 2>/dev/null; fail "1b: no READY"; }
[ "$(vcount_kd)" = 1 ] || { kill -KILL "$pb"; fail "1b: output not created"; }
kill -KILL "$pb"; wait "$pb" 2>/dev/null
wait_gone || fail "1b: output left behind 10 s after SIGKILL of its owner"
info "1b ok: SIGKILL of the owner removes the output"

# --- 1c. double create (same name, then different name): never two outputs from one request, no crash
"$VP" -k -n plh-virtual >"$D/c1.out" 2>&1 & p1=$!
wait_ready "$D/c1.out" || { kill -KILL "$p1"; fail "1c: first create failed"; }
"$VP" -k -n plh-virtual >"$D/c2.out" 2>&1 & p2=$!
sleep 2
n=$(vcount_kd)
kill -KILL "$p1" "$p2" 2>/dev/null; wait "$p1" "$p2" 2>/dev/null
wait_gone || fail "1c: output(s) left behind after killing both owners"
info "1c: two simultaneous creates with the same name gave $n output(s) (native KWin behaviour; the bridge must enforce at most one, see case 2)"
echo "  # NOTE: bridge-level 'at most one virtual output' is tested in case 2c once VOUT_CREATE exists (case 2)"

# --- 1d. flapping: 5 quick create/kill cycles leave nothing behind and do not grow kwinoutputconfig.json
cfg="$HOME/.config/kwinoutputconfig.json"
s0=$(stat -c %s "$cfg" 2>/dev/null || echo 0)
for i in 1 2 3 4 5; do
  "$VP" -k -n plh-virtual >"$D/f$i.out" 2>&1 & pf=$!
  wait_ready "$D/f$i.out" || { kill -KILL "$pf"; fail "1d: cycle $i no READY"; }
  kill -KILL "$pf"; wait "$pf" 2>/dev/null
  wait_gone || fail "1d: cycle $i left an output behind"
done
s1=$(stat -c %s "$cfg" 2>/dev/null || echo 0)
info "1d ok: 5 create/kill cycles, nothing left; kwinoutputconfig.json $s0 -> $s1 bytes"
[ $((s1 - s0)) -le 4096 ] || fail "1d: kwinoutputconfig.json grew by $((s1 - s0)) bytes in 5 cycles (unbounded growth of KWin config)"

# --- 3s. simulated xrandr: the REAL screen_layout() must not change when Virtual-plh* appears (any position)
if [ -f "$HOST" ] && grep -q 'Virtual-plh' "$HOST"; then
  awk '/^screen_layout\(\) \{/{p=1} p{print} p&&/^}/{exit}' "$HOST" >"$D/sl-sim.sh"
  mkdir -p "$D/bin"
  X3='Monitors: 3
 0: +*DP-2 1920/527x1080/296+0+0  DP-2
 1: +HDMI-A-2 1920/527x1080/296+1920+0  HDMI-A-2
 2: +HDMI-A-1 1920/527x1080/296+3840+0  HDMI-A-1'
  X4='Monitors: 4
 0: +*DP-2 1920/527x1080/296+0+0  DP-2
 1: +HDMI-A-2 1920/527x1080/296+1920+0  HDMI-A-2
 2: +HDMI-A-1 1920/527x1080/296+3840+0  HDMI-A-1
 3: +Virtual-plh-virtual 1920/1920x1080/1080+5760+0  Virtual-plh-virtual'
  X4m='Monitors: 4
 0: +*DP-2 1920/527x1080/296+0+0  DP-2
 1: +Virtual-plh-virtual 1920/1920x1080/1080+5760+0  Virtual-plh-virtual
 2: +HDMI-A-2 1920/527x1080/296+1920+0  HDMI-A-2
 3: +HDMI-A-1 1920/527x1080/296+3840+0  HDMI-A-1'
  X2='Monitors: 2
 0: +*DP-2 1920/527x1080/296+0+0  DP-2
 1: +HDMI-A-2 1920/527x1080/296+1920+0  HDMI-A-2'
  sim() { printf '#!/bin/sh\ncat <<"EOF"\n%s\nEOF\n' "$1" >"$D/bin/xrandr"; chmod +x "$D/bin/xrandr"
          PATH="$D/bin:$PATH" bash -c ". $D/sl-sim.sh; screen_layout"; }
  l3=$(sim "$X3"); l4=$(sim "$X4"); l4m=$(sim "$X4m"); l2=$(sim "$X2")
  [ -n "$l3" ] || fail "3s: screen_layout returned nothing for a 3-screen xrandr output"
  [ "$l3" = "$l4" ]  || fail "3s: virtual output appended changes screen_layout (host would restart)"
  [ "$l3" = "$l4m" ] || fail "3s: virtual output in the middle changes screen_layout (indexes shift)"
  [ "$l3" != "$l2" ] || fail "3s: removing a REAL screen is no longer detected (filter too greedy)"
  # a real output whose name merely contains 'Virtual' (e.g. a VM) must still count
  l5=$(sim "$(printf '%s\n 3: +Virtual-1 1024/300x768/200+5760+0  Virtual-1' "$X3")")
  [ "$l3" != "$l5" ] || fail "3s: a non-plh output named Virtual-1 is wrongly ignored"
  info "3s ok: real screen_layout ignores Virtual-plh* (appended or in the middle) but still sees real changes"
else
  note_skip "3s simulated screen_layout: host script does not filter Virtual-plh"
fi

# --- 3. layout-watch must not treat Virtual-plh* as a layout change
if [ -f "$HOST" ] && grep -q 'plh-virtual\|Virtual-plh' "$HOST"; then
  fn() { awk -v n="$1" '$0 ~ "^"n"\\(\\) \\{" {p=1} p {print} p && /^}/ {exit}' "$HOST"; }
  fn screen_layout >"$D/sl.sh"
  base=$(bash -c ". $D/sl.sh; screen_layout")
  "$VP" -k -n plh-virtual >"$D/w.out" 2>&1 & pw=$!
  wait_ready "$D/w.out" || { kill -KILL "$pw"; fail "3: no READY"; }
  with=$(bash -c ". $D/sl.sh; screen_layout")
  kill -KILL "$pw"; wait "$pw" 2>/dev/null; wait_gone
  [ "$base" = "$with" ] || fail "3: screen_layout differs while the virtual output exists (layout-watch would restart the host)"
  info "3 ok: screen_layout identical with and without the virtual output"
else
  note_skip "3 layout-watch ignoring plh-virtual: bin/parsec-linux-host does not mention it yet (lead). Currently a virtual output alive > ~8 s WOULD restart the host once no client is connected"
fi

# --- 2. bridge-level VOUT, placeholder in Wine, F2: need components not delivered yet
if grep -q 'VOUT_CREATE' "$REPO"/src/bridge/proto/*.h 2>/dev/null; then
  note_skip "2 bridge VOUT_CREATE/DESTROY/kill: proto has VOUT_CREATE but t42 has no client driver yet (needs tests/native/vout_client or a bridge CLI; asked capture)"
else
  note_skip "2 bridge VOUT_CREATE/DESTROY/bridge-killed: PLH_MSG_VOUT_* not in src/bridge/proto yet (capture)"
fi
note_skip "4 placeholder in EnumOutputs and user32 EnumDisplayDevicesW/EnumDisplaySettingsW (probe exe): dxgi placeholder and patch 0006 not delivered"
note_skip "5 duplicate -> frames -> release + 3 s grace -> gone; never listed twice: needs 4"
note_skip "6 F2 with PLH_FAKE_SCREENS_OFF=1: not delivered"
if [ "${PLH_TEST_INTRUSIVE:-0}" = 1 ]; then
  note_skip "7 input on the virtual output (flock /tmp/plh-input.lock, 50 px from LIVE edges): redirect/uinput resize not delivered"
else
  note_skip "7 input on the virtual output: intrusive (PLH_TEST_INTRUSIVE=1) and not delivered"
fi

if [ ${#SKIPS[@]} -gt 0 ]; then
  echo "  # native cases PASS; ${#SKIPS[@]} case(s) not run:"
  printf '  #   - %s\n' "${SKIPS[@]}"
  skip "native virtual output cases 1a-1d pass; ${#SKIPS[@]} case(s) waiting for components (listed above)"
fi
pass "virtual display: all cases"
