#!/bin/bash
# live-vout.sh — REAL test: the uinput absolute device follows the desktop when KWin's virtual output
# (plh-virtual, created by vout-probe) appears and disappears. TAP output.
# Injects real pointer motion + one click in OUR test window. Safety (CLAUDE.md "Input test safety"):
# flock /tmp/plh-input.lock; 50 px margin from the LIVE desktop edges; probe click in our window first
# (abort without further injection if not received); no key; skipped if a client is connected.
set -u
exec 8>/tmp/plh-input.lock
flock -w 120 8 || { echo "Bail out! /tmp/plh-input.lock unavailable"; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
ctl=$root/build/input/plh-input-ctl; probe=$root/build/bridge/vout-probe
d=/tmp/plh-entrees; mkdir -p "$d"
n=0; fails=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fails=$((fails+1)); echo "not ok $n - $*"; }
bail() { echo "Bail out! $*"; cleanup; exit 1; }
cleanup() {
  exec 9>&-
  [ "${ppid_:-0}" -gt 0 ] && kill "$ppid_" 2>/dev/null
  [ "${twpid:-0}" -gt 0 ] && kill "$twpid" 2>/dev/null
  wait 2>/dev/null
  return 0
}

if "$root/bin/parsec-linux-host" status 2>/dev/null | grep -E '^client' | grep -qv none; then echo "1..0 # SKIP a Parsec client is connected"; exit 0; fi
kdesk() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Geometry:/{split($2,p,",");split($3,s,"x");
  x=p[1];y=p[2];w=s[1];h=s[2]; if(!i||x<mx)mx=x; if(!i||y<my)my=y; if(!i||x+w>Mx)Mx=x+w; if(!i||y+h>My)My=y+h; i=1}
  END{print mx,my,Mx-mx,My-my}'; }
vgeom() { kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/^Output:/{v=($0 ~ /plh-virtual/)} v&&/Geometry:/{split($2,p,",");split($3,s,"x");print p[1],p[2],s[1],s[2]}'; }
for _ in $(seq 60); do [ -z "$(vgeom)" ] && break; sleep 1; done   # another teammate may be using it
vgeom0=$(vgeom); [ -z "$vgeom0" ] || bail "a virtual output already exists"
read -r DX DY DW DH < <(kdesk); echo "# live desktop $DX,$DY ${DW}x$DH"
orig=$("$here/kwin-cursor-pos.sh")

python3 "$here/testwin.py" --timeout 60 > "$d/tw.log" 2>&1 & twpid=$!
for _ in $(seq 60); do grep -q READY "$d/tw.log" 2>/dev/null && break; sleep 0.1; done
sleep 0.3
read -r WX WY WW WH WACT < <("$here/kwin-window-geom.sh" "PLH input test window")
[ -n "${WX:-}" ] || bail "test window not found"
CX=$((WX + WW/2)); CY=$((WY + WH/2))
[ "$CX" -ge $((DX+50)) ] && [ "$CX" -le $((DX+DW-51)) ] || bail "test window too close to an edge"

mkfifo "$d/vctl.fifo" 2>/dev/null || { rm -f "$d/vctl.fifo"; mkfifo "$d/vctl.fifo"; }
"$ctl" --desktop "$DX,$DY,${DW}x$DH" --settle 700 < "$d/vctl.fifo" > "$d/vctl.out" 2>&1 & ctlpid=$!
exec 9> "$d/vctl.fifo"
send() { echo "$*" >&9; }
active() { [ "$(grep -E '^(READY|INACTIVE)$' "$d/tw.log" | tail -1)" = READY ]; }
# abs X Y then check KWin's cursor; up to 3 tries (a human mouse move between inject and read is a false negative)
expect_abs() { local x=$1 y=$2 msg=$3 p=""
  for _ in 1 2 3; do send "abs $x $y"; sleep 0.45; p=$(curpos); [ "$p" = "$x $y" ] && { ok "$msg ($p)"; return; }; done
  nok "$msg: got '$p' want '$x $y'"; }
curpos() { "$here/kwin-cursor-pos.sh"; }

# --- probe click in our own window: abort without further injection if not received
sleep 1.2
active || bail "test window not active (human active?): no injection done"
for _ in 1 2 3; do send "abs $CX $CY"; sleep 0.8; [ "$(curpos)" = "$CX $CY" ] && break; done
[ "$(curpos)" = "$CX $CY" ] || bail "cursor not at the window centre after abs: abort"
active || bail "test window lost focus: abort"
send "btn left click"; sleep 0.6
grep -q '^BTN down 1' "$d/tw.log" && ok "probe click received in our window" || bail "probe click NOT received: abort"

# --- virtual output appears
"$probe" -t 14 > "$d/probe.out" 2>&1 & ppid_=$!
for _ in $(seq 100); do [ -n "$(vgeom)" ] && break; sleep 0.1; done
# KWin first shows the output at 0,0 for a moment, then places it: wait for a stable geometry
prev=""; for _ in $(seq 40); do cur="$(vgeom) $(kdesk)"; [ "$cur" = "$prev" ] && break; prev="$cur"; sleep 0.6; done
read -r VX VY VW VH < <(vgeom)
[ -n "${VX:-}" ] && ok "virtual output present: $VX,$VY ${VW}x$VH" || bail "virtual output did not appear"
read -r NX NY NW NH < <(kdesk)
echo "# desktop with virtual output: $NX,$NY ${NW}x$NH"
[ "$NW" -gt "$DW" ] && ok "desktop grew ($DW -> $NW)" || nok "desktop did not grow"

# before resize the old range cannot reach the virtual output: clamped to the old edge, must NOT land on it
vcx=$((VX+VW/2)); vcy=$((VY+VH/2))
send "resize $NX $NY $NW $NH"; sleep 1.5
grep -q '^RESIZE.*kwin-discovered=yes' "$d/vctl.out" && ok "absolute device recreated and discovered by KWin: $(grep '^RESIZE' "$d/vctl.out")" || nok "resize/discovery: $(cat "$d/vctl.out")"
expect_abs $vcx $vcy "abs at the virtual output centre exact"
# border between the last real screen and the virtual output (never an outer edge)
if [ "$VX" -gt "$NX" ]; then bx=$((VX-1)); else bx=$((VX+VW)); fi   # KWin may place it on the left edge
expect_abs $bx $((VY+VH/3)) "abs just across the border from the virtual output exact"
expect_abs $VX $((VY+VH/3)) "abs on the first virtual column exact"
# previous range still correct: centre of a real screen
expect_abs $CX $CY "abs back on the real desktop exact"

# --- virtual output goes away
wait "$ppid_" 2>/dev/null; ppid_=0
for _ in $(seq 50); do [ -z "$(vgeom)" ] && break; sleep 0.1; done
[ -z "$(vgeom)" ] && ok "virtual output removed" || nok "virtual output still there"
send "resize $DX $DY $DW $DH"; sleep 1.5
expect_abs $CX $CY "after shrink: abs at window centre exact"
expect_abs $((DX+DW-51)) $CY "after shrink: abs 50 px from the new right edge exact (range follows the live desktop)"
active && { send "abs $CX $CY"; sleep 0.4; send "btn left click"; sleep 0.5; [ "$(grep -c '^BTN down 1' "$d/tw.log")" -ge 2 ] && ok "click still lands in our window after the shrink" || nok "click after shrink"; } || echo "# skip final click: window not active"
send "abs $orig"; sleep 0.3
cleanup
echo "# cursor restored: $orig -> $(curpos)"
echo "1..$n"
exit $((fails != 0))
