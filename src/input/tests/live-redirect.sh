#!/bin/bash
# live-redirect.sh — F2 receiving side end to end (owner: entrees). TAP output.
# Private bridge (input service ON, all real screens reported off, AUTO virtual display) + the capture
# probe vd_probe.exe under Wine duplicating the default output: the proxy pushes INPUT_REDIRECT
# (Windows 0,0,1920x1080 -> virtual output). A SEPARATE input connection (plh-bridge-inject, like the hook)
# then sends an absolute move at Windows (960,540): KWin's cursor must land on the virtual output centre, and
# after release + grace (output gone, table dropped with the owner connection) the same move must land on the
# real screen again. Injects real pointer motion + one click in OUR window. Safety: input lock, vout lock, host lock,
# no client, probe click first, 50 px margins from the LIVE edges, no key.
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd); cd "$R" || exit 2
d=/tmp/plh-entrees; mkdir -p "$d"
n=0; fails=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fails=$((fails+1)); echo "not ok $n - $*"; }
exec 8>/tmp/plh-input.lock;  flock -w 120 8 || { echo "Bail out! input lock"; exit 1; }
exec 7>/tmp/plh-vout.lock;   flock -w 180 7 || { echo "Bail out! vout lock"; exit 1; }
exec 6>/tmp/plh-host.lock;   flock -w 300 6 || { echo "Bail out! host lock"; exit 1; }
bridge_pid=0; twpid=0; probe_pid=0; injpid=0
cleanup() {
  exec 9>&- 2>/dev/null
  for p in $probe_pid $injpid $bridge_pid $twpid; do [ "$p" -gt 0 ] && kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  [ -n "${WINESERVER:-}" ] && "$WINESERVER" -k 2>/dev/null
  rm -rf "${RD:-/nonexistent-plh}"
  unset PLH_BRIDGE_DIR
  flock -u 6; flock -u 8; flock -u 7      # drop locks before the host start so its daemons do not need them
  bin/parsec-linux-host start >/dev/null 2>&1 6>&- 7>&- 8>&- 9>&-
  return 0
}
bail() { echo "Bail out! $*"; cleanup; exit 1; }

bin/parsec-linux-host status | grep -q 'client *none' || { echo "1..0 # SKIP a Parsec client is connected"; exit 0; }
xrandr --listmonitors 2>/dev/null | grep -q Virtual-plh && { echo "1..0 # SKIP a virtual output already exists"; exit 0; }
x86_64-w64-mingw32-gcc -std=gnu11 -Wall -Wextra -o $d/vd_probe.exe src/capture/dxgi/test/vd_probe.c -ldxgi -ld3d11 -ldxguid -luuid -luser32 || exit 2
make -s -C src/input >/dev/null 2>&1

vgeom() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/^Output:/{v=($0 ~ /plh-virtual/)} v&&/Geometry:/{split($2,p,",");split($3,s,"x");print p[1],p[2],s[1],s[2]}'; }
kdesk() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Geometry:/{split($2,p,",");split($3,s,"x");
  x=p[1];y=p[2];w=s[1];h=s[2]; if(!i||x<mx)mx=x; if(!i||y<my)my=y; if(!i||x+w>Mx)Mx=x+w; if(!i||y+h>My)My=y+h; i=1}
  END{print mx,my,Mx-mx,My-my}'; }
# the bridge's default Windows->KWin translation = position of the KWin primary output (priority 1)
read -r PX PY < <(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/priority 1/{p=1} p&&/Geometry:/{split($2,q,",");print q[1],q[2];exit}')
read -r DX DY DW DH < <(kdesk); echo "# live desktop $DX,$DY ${DW}x$DH; translation $PX,$PY"
orig=$("$R/src/input/tests/kwin-cursor-pos.sh")
here=$R/src/input/tests
curpos() { "$here/kwin-cursor-pos.sh"; }

# private bridge, host stopped (so the proxy talks to it); never the host's runtime dir
bin/parsec-linux-host stop >/dev/null 2>&1 6>&- 7>&- 8>&-; sleep 2
bash src/capture/dxgi/install-proxy.sh >/dev/null 6>&- 7>&- 8>&-
. runtime/env.sh
RD=$d/rt-redirect; rm -rf $RD; mkdir -m 700 -p $RD
export PLH_CAPTURE_BACKEND=kwin PLH_INPUT_BACKEND=uinput PLH_BRIDGE_DIR=$RD
export PLH_VIRTUAL_DISPLAY=0 PLH_VIRTUAL_DISPLAY_AUTO=1 PLH_FAKE_SCREENS_OFF=1
build/bridge/plh-bridge --runtime-dir $RD -v > $d/redirect.bridge.log 2>&1 6>&- 7>&- 8>&- & bridge_pid=$!
for _ in $(seq 50); do [ -S $RD/bridge.sock ] && [ -s $RD/port ] && break; sleep 0.1; done
# The bridge logs its services just after creating the socket: wait for the line, not just the files.
for _ in $(seq 100); do grep -q 'service input: active' $d/redirect.bridge.log && break; sleep 0.1; done
grep -q 'service input: active' $d/redirect.bridge.log || bail "input service not active in the private bridge"

python3 "$here/testwin.py" --timeout 90 > "$d/tw.log" 2>&1 6>&- 7>&- 8>&- & twpid=$!
for _ in $(seq 60); do grep -q READY "$d/tw.log" 2>/dev/null && break; sleep 0.1; done
sleep 0.3
read -r WX WY WW WH WACT < <("$here/kwin-window-geom.sh" "PLH input test window")
[ -n "${WX:-}" ] || bail "test window not found"
CX=$((WX + WW/2)); CY=$((WY + WH/2))
[ "$CX" -ge $((DX+50)) ] && [ "$CX" -le $((DX+DW-51)) ] || bail "test window too close to an edge"
active() { [ "$(grep -E '^(READY|INACTIVE)$' "$d/tw.log" | tail -1)" = READY ]; }

mkfifo $d/inj.fifo 2>/dev/null || { rm -f $d/inj.fifo; mkfifo $d/inj.fifo; }
build/input/plh-bridge-inject < $d/inj.fifo > $d/inj.out 2>&1 6>&- 7>&- 8>&- & injpid=$!
exec 9> $d/inj.fifo
send() { echo "$*" >&9; }
sleep 1.5      # let KWin discover the bridge's new uinput devices
# expect_abs WX WY KX KY msg : send a Windows-space move, KWin cursor must be at KX KY (3 tries: a human mouse move is a false negative)
expect_abs() { local p=""
  for _ in 1 2 3; do send "abs $1 $2"; sleep 0.5; p=$(curpos); [ "$p" = "$3 $4" ] && { ok "$5 ($p)"; return; }; done
  nok "$5: got '$p' want '$3 $4'"; }

# probe click in our window (Windows coords = KWin - translation); abort without further injection otherwise
active || bail "test window not active (human active?): no injection done"
for _ in 1 2 3; do send "abs $((CX-PX)) $((CY-PY))"; sleep 0.6; [ "$(curpos)" = "$CX $CY" ] && break; done
[ "$(curpos)" = "$CX $CY" ] || bail "cursor not at the window centre: abort"
active || bail "test window lost focus: abort"
send "click"; sleep 0.6
grep -q '^BTN down 1' "$d/tw.log" && ok "probe click received in our window (through the bridge input path)" || bail "probe click NOT received: abort"

# no redirect yet: Windows (960,540) is an ordinary move
expect_abs 960 540 $((960+PX)) $((540+PY)) "before F2: Windows 960,540 -> real screen (translation only)"

# F2: the proxy duplicates the default output -> virtual output + redirect
timeout 80 wine $d/vd_probe.exe 22 f2 2>&1 6>&- 7>&- 8>&- | tr -d '\r' > $d/redirect.probe.out & probe_pid=$!
for _ in $(seq 150); do [ -n "$(vgeom)" ] && grep -q 'redirect entries' $d/redirect.bridge.log && break; sleep 0.2; done
prev=""; for _ in $(seq 40); do cur="$(vgeom) $(kdesk)"; [ "$cur" = "$prev" ] && break; prev="$cur"; sleep 0.6; done
read -r VX VY VW VH < <(vgeom)
[ -n "${VX:-}" ] && ok "virtual output present: $VX,$VY ${VW}x$VH" || bail "no virtual output"
grep -q 'client [0-9]*: 1 redirect entries' $d/redirect.bridge.log && ok "bridge stored 1 redirect entry from the proxy connection" || nok "no redirect entry in the bridge log"
active || bail "test window lost focus (Wine window?): abort"
vcx=$((VX+VW/2)); vcy=$((VY+VH/2))
expect_abs 960 540 $vcx $vcy "F2: Windows 960,540 (default output centre) -> virtual output centre"
expect_abs 100 80 $((VX+100)) $((VY+80)) "F2: Windows 100,80 -> same offset on the virtual output"
expect_abs 1919 1079 $((VX+1919)) $((VY+1079)) "F2: Windows 1919,1079 (last pixel of the rect) -> last pixel of the virtual output"
# outside the redirected rect: ordinary translation (second Windows screen on the real desktop)
expect_abs -960 540 $((-960+PX)) $((540+PY)) "F2: Windows -960,540 (outside the rect) -> translation only"

# release + grace: output removed, table dropped
wait $probe_pid 2>/dev/null; probe_pid=0
for _ in $(seq 60); do [ -z "$(vgeom)" ] && break; sleep 0.2; done
[ -z "$(vgeom)" ] && ok "virtual output removed after release + grace" || nok "virtual output still there"
grep -q 'redirect entries of a closed connection dropped' $d/redirect.bridge.log && ok "redirect table dropped with its owner connection" || nok "no 'dropped' line in the bridge log"
sleep 1
expect_abs 960 540 $((960+PX)) $((540+PY)) "after release: Windows 960,540 -> real screen again (not redirected)"
send "abs $((${orig% *}-PX)) $((${orig#* }-PY))"; sleep 0.3
cleanup
echo "# cursor restored: $orig -> $(curpos)"
for l in vout input host; do
  ( flock -n 3 ) 3>/tmp/plh-$l.lock && ok "lock /tmp/plh-$l.lock not leaked" || nok "lock /tmp/plh-$l.lock leaked: $(fuser /tmp/plh-$l.lock 2>&1 | tr -s ' ')"
done
echo "1..$n"
exit $((fails != 0))
