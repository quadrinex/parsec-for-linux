#!/bin/bash
# live-uinput.sh — REAL test of the uinput backend in the KDE Wayland session (injects real input,
# harmless: motion, clicks/wheel on the test window, F24 and Left Shift keys only
# and only if the test window has focus). TAP output. Restores the cursor position.
# Usage: live-uinput.sh [path/plh-input-ctl]
set -u
# One injector at a time (CLAUDE.md, "Input test safety").
exec 8>/tmp/plh-input.lock
flock -w 120 8 || { echo "Bail out! verrou /tmp/plh-input.lock indisponible"; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
ctl=${1:-$here/../../../build/input/plh-input-ctl}
d=/tmp/plh-entrees; mkdir -p "$d"
n=0; fails=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fails=$((fails+1)); echo "not ok $n - $*"; }

# Desktop: bounding rectangle from kscreen-doctor
read -r DX DY DW DH < <(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Geometry:/{split($2,p,",");split($3,s,"x");
  x=p[1];y=p[2];w=s[1];h=s[2]; if(!i||x<mx)mx=x; if(!i||y<my)my=y; if(!i||x+w>Mx)Mx=x+w; if(!i||y+h>My)My=y+h; i=1}
  END{print mx,my,Mx-mx,My-my}')
echo "# desktop ${DX},${DY} ${DW}x${DH}"
orig=$("$here/kwin-cursor-pos.sh")

# KWin monitor of all cursor positions
cat > "$d/mon.js" <<'JS'
workspace.cursorPosChanged.connect(function() { print("PLHMON " + workspace.cursorPos.x + " " + workspace.cursorPos.y); });
JS
since=$(date '+%Y-%m-%d %H:%M:%S')
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript plhmon >/dev/null 2>&1
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$d/mon.js" plhmon)
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null

# Test window
python3 "$here/testwin.py" --timeout 25 > "$d/tw.log" 2>&1 & twpid=$!
for _ in $(seq 60); do grep -q READY "$d/tw.log" 2>/dev/null && break; sleep 0.1; done
sleep 0.3
read -r WX WY WW WH WACT < <("$here/kwin-window-geom.sh" "PLH input test window")
echo "# test window ${WX},${WY} ${WW}x${WH} active=$WACT"
CX=$((WX + WW/2)); CY=$((WY + WH/2))

# Absolute points: centre of each screen, borders BETWEEN screens, and near the desktop corners
# with a margin of M px. NEVER touch the outer desktop edges: an edge can be
# a capture "barrier" (KDE Connect "share input devices", InputCapture)
# that would then send all of the human's input to another device.
M=50
pts=("$((DX+M)) $((DY+M))" "$((DX+DW-1-M)) $((DY+DH-1-M))")
while read -r gx gy gw gh; do
  pts+=("$((gx+gw/2)) $((gy+gh/2))")
  [ "$gx" -gt "$DX" ] && pts+=("$((gx-1)) $((gy+gh/3))" "$gx $((gy+gh/3))")
done < <(kscreen-doctor -o 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | awk '/Geometry:/{split($2,p,",");split($3,s,"x");print p[1],p[2],s[1],s[2]}')

# Injection driven line by line: check the test window's focus before clicks/keys,
# so as never to click/type in another window if the human changes focus during the test.
mkfifo "$d/ctl.fifo" 2>/dev/null || { rm -f "$d/ctl.fifo"; mkfifo "$d/ctl.fifo"; }
"$ctl" --desktop "$DX,$DY,${DW}x$DH" --settle 700 < "$d/ctl.fifo" & ctlpid=$!
exec 9> "$d/ctl.fifo"
send() { echo "$*" >&9; }
active() { [ "$(grep -E '^(READY|INACTIVE)$' "$d/tw.log" | tail -1)" = READY ]; }
under_cursor() { [ "$("$here/kwin-cursor-pos.sh")" = "$CX $CY" ]; }
for p in "${pts[@]}"; do send "abs $p"; send "sleep 40"; done
send "abs $CX $CY"; send "sleep 100"
send "rel 10 0"; send "sleep 60"; send "rel 0 10"; send "sleep 60"; send "abs $CX $CY"; send "sleep 1000"
sleep 2.5   # let the queue drain (settle 700 ms + ~1.4 s)
PTR_OK=0; KEY_OK=0
if active && under_cursor; then
  PTR_OK=1
  send "btn left click"; send "sleep 40"; send "btn right click"; send "sleep 40"; send "btn middle click"; send "sleep 40"
  send "wheel 120 0"; send "sleep 40"; send "wheel -120 0"; send "sleep 40"; send "wheel 0 120"; send "sleep 40"
  send "wheel 60 0"; send "sleep 40"; send "wheel 60 0"; send "sleep 300"
  sleep 0.8
fi
if active; then
  KEY_OK=1
  send "key F24 tap"; send "sleep 40"; send "key LEFTSHIFT tap"; send "sleep 40"
  send "sc 0x76 down"; send "sleep 20"; send "sc 0x76 up"; send "sleep 40"     # scancode Windows F24
  send "vk 0x87 down"; send "sleep 20"; send "vk 0x87 up"; send "sleep 40"     # VK_F24
  send "sc 0x1d e0 down"; send "sleep 20"; send "sc 0x1d e0 up"; send "sleep 40" # Ctrl droit (E0 1D)
fi
send "sleep 200"; send "abs $orig"
exec 9>&-
wait "$ctlpid"; rc=$?
sleep 0.3
kill "$twpid" 2>/dev/null; wait "$twpid" 2>/dev/null
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript plhmon >/dev/null
journalctl --user --since "$since" -o cat | grep '^PLHMON' | sed 's/^PLHMON //' > "$d/mon.log"
[ $rc = 0 ] && ok "plh-input-ctl runs without error" || nok "plh-input-ctl code $rc"

for p in "${pts[@]}"; do
  if grep -qx "$p" "$d/mon.log"; then ok "abs -> KWin cursor exactly at $p"; else nok "abs $p not observed"; fi
done
# relative: positions after "rel 10 0" / "rel 0 10" from the window centre
line=$(grep -nx "$CX $CY" "$d/mon.log" | head -1 | cut -d: -f1)
r1=$(sed -n "$((line+1))p" "$d/mon.log"); r2=$(sed -n "$((line+2))p" "$d/mon.log")
echo "# rel: depuis $CX $CY -> '$r1' -> '$r2'"
read -r r1x r1y <<<"$r1"; read -r r2x r2y <<<"$r2"
# flat profile set by plh-input-ctl through KWin DBus: motion must be exactly 1:1
if [ "$r1" = "$((CX+10)) $CY" ]; then ok "rel +10,0 -> exactly +10 px (no acceleration)"; else nok "rel +10,0 -> '$r1' (expected $((CX+10)) $CY)"; fi
if [ "$r2" = "$((CX+10)) $((CY+10))" ]; then ok "rel 0,+10 -> exactement +10 px"; else nok "rel 0,+10 -> '$r2'"; fi

tw=$(cat "$d/tw.log"); echo "$tw" | sed 's/^/# testwin: /'
chk() { if grep -q "$1" <<<"$tw"; then ok "$2"; else nok "$2"; fi; }
: # absolute return to the same point after relative moves
if [ "$(grep -cx "$CX $CY" "$d/mon.log")" -ge 2 ]; then ok "abs to the same point after relative: cursor back at $CX $CY"; else nok "repeated abs $CX $CY not observed"; fi
if [ $PTR_OK = 1 ]; then
chk "^BTN down 1 $((WW/2)) $((WH/2))" "left click received at the exact window centre"
chk "^BTN down 2 " "right click received"
chk "^BTN down 4 " "middle click received"
chk "^WHEEL 120 0" "wheel up = +120"
chk "^WHEEL -120 0" "wheel down = -120"
chk "^WHEEL 0 -120\|^WHEEL 0 120" "horizontal wheel received"
chk "^WHEEL 60 0" "high-resolution wheel (half notch = 60)"
else nok "# SKIP buttons/wheel: test window inactive or no longer under the cursor (human active?)"; fi
if [ $KEY_OK = 1 ]; then
  [ "$(grep -c '^KEY down 194 $' <<<"$tw")" -ge 3 ] && ok "F24 received 3 times (name, scancode 0x76, VK_F24)" || nok "F24 x3"
  chk "^KEY up 194" "F24 release"
  chk "^KEY down 42 " "Left Shift (KEY_LEFTSHIFT=42)"
  chk "^KEY down 97 " "scancode E0 1D -> Right Ctrl (KEY_RIGHTCTRL=97)"
else
  nok "# SKIP keyboard: the test window no longer had focus (human active?)"
fi
now=$("$here/kwin-cursor-pos.sh"); echo "# cursor restored : $orig -> $now"
echo "1..$n"
exit $((fails != 0))
