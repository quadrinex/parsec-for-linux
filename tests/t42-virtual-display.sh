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

# One virtual-output user at a time: the output is a shared, session-wide resource (other teammates' live tests
# create it too, which makes counts flaky). Convention: hold /tmp/plh-vout.lock while creating one.
exec 9>/tmp/plh-vout.lock
flock -w 180 9 || skip "/tmp/plh-vout.lock busy for 180 s (another test is using the virtual output)"
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

# --- 2. bridge-level VOUT (private bridge: own runtime dir, no contact with the live host's bridge)
VT="$REPO/build/bridge/plh-vout-test"; BR="$REPO/build/bridge/plh-bridge"
if [ -x "$VT" ] && [ -x "$BR" ]; then
  RD=$(mktemp -d "$D/rt.XXXXXX"); chmod 700 "$RD"
  start_br() { # env passed through; sets BP
    ( exec env PLH_BRIDGE_DIR=$RD PLH_INPUT_BACKEND=none "$BR" --runtime-dir "$RD" >>"$RD/log" 2>&1 ) & BP=$!
    local i; for i in $(seq 60); do [ -S "$RD/bridge.sock" ] && [ -s "$RD/port" ] && return 0; sleep 0.1; done
    return 1; }
  vt() { PLH_BRIDGE_DIR=$RD "$VT" "$@"; }
  stop_br() { kill "$BP" 2>/dev/null; wait "$BP" 2>/dev/null; rm -f "$RD/bridge.sock" "$RD/port"; }
  cleanup_br() { kill -KILL "$BP" "$O1" "$O2" 2>/dev/null; }
  O1=; O2=; BP=
  trap cleanup_br EXIT

  # 2a. both options off -> VOUT_CREATE refused, nothing created (R3, fixed by capture a0b85ee); AUTO alone is enough to allow it
  PLH_VIRTUAL_DISPLAY=0 PLH_VIRTUAL_DISPLAY_AUTO=0 start_br || fail "2a: private bridge did not start: $(tail -3 "$RD/log")"
  if vt create -t 1 >"$D/2a.out" 2>&1; then rc=0; else rc=$?; fi
  [ "$(vcount_kd)" = 0 ] || fail "2a: virtual output created although both options are off"
  [ "$rc" != 0 ] || fail "2a: create reported success with both options off: $(head -2 "$D/2a.out" | tr '\n' ' ')"
  stop_br
  PLH_VIRTUAL_DISPLAY=0 PLH_VIRTUAL_DISPLAY_AUTO=1 start_br || fail "2a: restart (AUTO) failed"
  vt create -t 1 >"$D/2a2.out" 2>&1 || fail "2a: create refused with PLH_VIRTUAL_DISPLAY_AUTO=1: $(tail -2 "$D/2a2.out")"
  wait_gone || fail "2a: output left behind"
  stop_br; info "2a ok: both off -> refused (rc=$rc, nothing created); AUTO alone -> allowed"

  PLH_VIRTUAL_DISPLAY=1 start_br || fail "2: private bridge did not start"
  # 2b. two owners: output stays until the LAST owner is gone; never two outputs
  ( exec env PLH_BRIDGE_DIR=$RD "$VT" create -k >"$D/2b1.out" 2>&1 ) & O1=$!
  wait_ready "$D/2b1.out" || fail "2b: first create no READY: $(tail -2 "$D/2b1.out")"
  ( exec env PLH_BRIDGE_DIR=$RD "$VT" create -k >"$D/2b2.out" 2>&1 ) & O2=$!
  wait_ready "$D/2b2.out" || fail "2b: second create no READY: $(tail -2 "$D/2b2.out")"
  [ "$(vcount_kd)" = 1 ] && [ "$(vcount_xr)" = 1 ] || fail "2b: double create gave $(vcount_kd) kscreen / $(vcount_xr) xrandr outputs (must be exactly 1)"
  id1=$(sed -n 's/^VOUT id=\([0-9]*\).*/\1/p' "$D/2b1.out"); id2=$(sed -n 's/^VOUT id=\([0-9]*\).*/\1/p' "$D/2b2.out")
  [ -n "$id1" ] && [ "$id1" = "$id2" ] || fail "2b: second create returned a different id ($id1 vs $id2)"
  kill -KILL "$O1"; wait "$O1" 2>/dev/null; sleep 1.5
  [ "$(vcount_kd)" = 1 ] || fail "2b: output removed while another owner is still connected"
  kill -KILL "$O2"; wait "$O2" 2>/dev/null
  wait_gone || fail "2b: output left behind after the last owner was SIGKILLed"
  info "2b ok: one output for two owners, survives the first owner's death, gone after the last"

  # 2c. a destroy from a non-owner must not remove the output of the real owner
  ( exec env PLH_BRIDGE_DIR=$RD "$VT" create -k >"$D/2c.out" 2>&1 ) & O1=$!
  wait_ready "$D/2c.out" || fail "2c: no READY"
  vt create -t 1 -d >"$D/2c2.out" 2>&1 || true
  sleep 0.5
  [ "$(vcount_kd)" = 1 ] || fail "2c: a second client's VOUT_DESTROY removed the output while the first owner still holds it"
  kill -KILL "$O1"; wait "$O1" 2>/dev/null; wait_gone || fail "2c: output left behind"
  info "2c ok: destroy by one owner keeps the output for the other"

  # 2d. flapping through the bridge: 6 create/destroy cycles, no leak, no growth of the bridge's fd count
  fd0=$(ls /proc/$BP/fd | wc -l)
  for i in 1 2 3 4 5 6; do
    vt create -t 1 -d >"$D/2d.$i" 2>&1 || fail "2d: cycle $i create failed: $(tail -2 "$D/2d.$i")"
    wait_gone || fail "2d: cycle $i left an output behind"
  done
  fd1=$(ls /proc/$BP/fd | wc -l)
  [ "$fd1" -le $((fd0 + 2)) ] || fail "2d: bridge fd count grew $fd0 -> $fd1 over 6 create/destroy cycles (leak)"
  info "2d ok: 6 create/destroy cycles clean, bridge fds $fd0 -> $fd1"

  # 2e. frames: a stream on the virtual output delivers frames (damage driven: expect a few)
  if vt create -t 2 -f -d >"$D/2e.out" 2>&1; then
    fr=$(sed -n 's/^FRAMES \([0-9]*\).*/\1/p' "$D/2e.out" | tail -1)
    [ "${fr:-0}" -ge 1 ] || fail "2e: no frame received from the virtual output: $(tail -3 "$D/2e.out" | tr '\n' ' ')"
    info "2e ok: $fr frame(s) received (damage driven)"
  else fail "2e: create -f failed: $(tail -3 "$D/2e.out" | tr '\n' ' ')"; fi
  wait_gone || fail "2e: output left behind after the capture stream test"

  # 2f. bridge SIGKILL while an owner holds the output -> KWin drops it
  ( exec env PLH_BRIDGE_DIR=$RD "$VT" create -k >"$D/2f.out" 2>&1 ) & O1=$!
  wait_ready "$D/2f.out" || fail "2f: no READY"
  kill -KILL "$BP"; wait "$BP" 2>/dev/null
  wait_gone || fail "2f: output left behind after SIGKILL of the bridge"
  kill -KILL "$O1" 2>/dev/null; wait "$O1" 2>/dev/null
  info "2f ok: bridge SIGKILL removes the output"
  rm -f "$RD/bridge.sock" "$RD/port"

  # 2g. SIGTERM of the bridge removes it too
  PLH_VIRTUAL_DISPLAY=1 start_br || fail "2g: restart failed"
  ( exec env PLH_BRIDGE_DIR=$RD "$VT" create -k >"$D/2g.out" 2>&1 ) & O1=$!
  wait_ready "$D/2g.out" || fail "2g: no READY"
  kill -TERM "$BP"; wait "$BP" 2>/dev/null
  wait_gone || fail "2g: output left behind after SIGTERM of the bridge"
  kill -KILL "$O1" 2>/dev/null; wait "$O1" 2>/dev/null
  info "2g ok: bridge SIGTERM removes the output"
  rm -f "$RD/bridge.sock" "$RD/port"

  # 6a. F2 state only: PLH_FAKE_SCREENS_OFF=1 is reported in DISPLAY_STATE
  PLH_VIRTUAL_DISPLAY=1 PLH_FAKE_SCREENS_OFF=1 start_br || fail "6a: restart failed"
  st=$(vt state); case $st in *fake_off=1*real_on=0*|*real_on=0*fake_off=1*) info "6a ok: DISPLAY_STATE reports screens off: $st";; *) fail "6a: fake screens-off not reported: $st";; esac
  stop_br
  trap - EXIT
  [ "$(vcount_kd)" = 0 ] || fail "2: a virtual output remains at the end"
else
  note_skip "2 bridge VOUT: build/bridge/plh-vout-test or plh-bridge missing (make -C src/bridge)"
fi
# --- 4. user32 placeholder (patch 0006) in a THROWAWAY prefix, with the real displaylist probe
DL="$BUILD/pe/displaylist.exe"
if [ ! -f "$DL" ]; then note_skip "4 placeholder in user32: $DL not built (mingw missing?)"
elif ! ( wine_env; command -v "$WINE" >/dev/null ); then note_skip "4 placeholder in user32: wine not found"
else
  (
  wine_env
  export WINEPREFIX="$D/pfx" WINEDLLOVERRIDES=winemenubuilder.exe=d
  trap '"${WINESERVER:-wineserver}" -k 2>/dev/null' EXIT
  K='HKLM\Software\Wine\PlhVirtualDisplay'
  dl() { timeout 60 "$WINE" "$DL" "$@" 2>&1 | tr -d '\r'; }
  base=$(dl | sed -n 's/^snap n=\([0-9]*\).*/\1/p')
  [ "${base:-0}" -ge 1 ] || { echo "4: probe got no adapter list"; exit 1; }
  # Windows x of the future output = right edge of the rightmost real adapter
  edge=$(dl | grep '^snap' | grep -o '@[-0-9]*,[0-9]*:[0-9]*x' | awk -F'[@,:x]' '{e=$2+$4; if(e>m)m=e} END{print m+0}')
  nm="\\\\.\\DISPLAY$((base+1))"
  reg() { "$WINE" reg add "$K" /v "$1" /t "$2" /d "$3" /f >/dev/null; }
  reg DeviceName REG_SZ "$nm"; reg Left REG_DWORD "$edge"; reg Top REG_DWORD 0; reg Width REG_DWORD 1920; reg Height REG_DWORD 1080; reg Active REG_DWORD 0
  # trans <tag>: poll in a message-pumping probe for 16 s; the real output exists from t=5 s to t=~14 s
  trans() { dl 16 >"$D/$1.dl" & local dp=$!; sleep 5; "$VP" -t 9 -n plh-virtual >"$D/$1.vp" 2>&1; wait "$dp"; o=$(cat "$D/$1.dl"); }
  # 4a. placeholder offered: exactly one extra adapter, right of the real ones, name = the real output's future name
  o=$(dl); n=$(echo "$o" | sed -n 's/^snap n=\([0-9]*\).*/\1/p')
  [ "$n" = $((base+1)) ] || { echo "4a: expected $((base+1)) adapters with the placeholder, got $n: $o"; exit 1; }
  echo "$o" | grep -qF "$nm@$edge,0:1920x1080" || { echo "4a: placeholder not at $edge,0 1920x1080 under $nm: $o"; exit 1; }
  echo "  # 4a ok: placeholder = $nm at $edge,0 (adapters $base -> $n)"
  # 4b. the real output appears while polling: never two entries for the same name/position at any instant
  trans 4b
  echo "$o" | grep -q '^dup=0' || { echo "4b: DUPLICATE display seen while the real output appeared: $o"; exit 1; }
  last=$(echo "$o" | grep '^snap' | tail -1)
  echo "$o" | grep -q "snap n=$((base+1))" || { echo "4b: adapter count never reached $((base+1)) with the real output: $o"; exit 1; }
  echo "  # 4b ok: no duplicate across the placeholder -> real -> placeholder transitions (identical list before/after is expected: the placeholder sits exactly where Wine puts the real output)"
  # 4c. step aside / key removed: back to the plain list
  reg Active REG_DWORD 1
  n=$(dl | sed -n 's/^snap n=\([0-9]*\).*/\1/p')
  [ "$n" = "$base" ] || { echo "4c: Active=1 must restore $base adapters, got $n"; exit 1; }
  "$WINE" reg delete "$K" /f >/dev/null 2>&1
  n=$(dl | sed -n 's/^snap n=\([0-9]*\).*/\1/p')
  [ "$n" = "$base" ] || { echo "4c: key removed must give $base adapters, got $n"; exit 1; }
  echo "  # 4c ok: Active=1 and key removal restore the plain list"
  # 4d. control for 4b: with NO key, Wine alone must list the real output (else 4b proved nothing about the transition)
  trans 4d
  echo "$o" | grep -q "max=$((base+1))" || { echo "4d: Wine never listed the real output (max adapters != $((base+1))): $o"; exit 1; }
  echo "  # 4d ok: control: Wine itself lists the real output ($nm) while it exists, so 4b really crossed the transition"
  ) ; rc=$?
  "$WINESERVER" -k 2>/dev/null || true
  [ $rc = 0 ] || fail "4: placeholder case failed (see lines above)"
  wait_gone || fail "4: output left behind"
fi
note_skip "5 duplicate -> frames -> release + 3 s grace -> gone: dxgi side (DuplicateOutput on the placeholder) not delivered"
note_skip "6b F2 behaviour (duplicate default output yields virtual frames + redirect): dxgi side not delivered; only the state flag (6a) is tested"
if [ "${PLH_TEST_INTRUSIVE:-0}" = 1 ]; then
  note_skip "7 input on the virtual output (flock /tmp/plh-input.lock, 50 px from LIVE edges): redirect/uinput resize not delivered"
else
  note_skip "7 input on the virtual output: intrusive (PLH_TEST_INTRUSIVE=1) and not delivered"
fi

if [ ${#SKIPS[@]} -gt 0 ]; then
  echo "  # native and bridge cases PASS; ${#SKIPS[@]} case(s) not run:"
  printf '  #   - %s\n' "${SKIPS[@]}"
  skip "native + bridge cases 1a-1d, 2a-2g, 6a pass; ${#SKIPS[@]} case(s) waiting for components (listed above)"
fi
pass "virtual display: all cases"
