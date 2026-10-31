#!/bin/bash
# e2e-hook-moves.sh — full input chain. Motion, then (with --full) clicks/wheel/F24, Shift and
# Right Ctrl keys delivered ONLY to a Wayland test window (focus and position checked
# beforehand; otherwise SKIP):
#   SendInput sous Wine (injectprobe.exe) -> plh-input-hook.exe (LL hooks, swallows) -> plh-bridge
#   (input service, TCP+token) -> uinput -> KWin ; positions observed by a KWin script.
# Prefix: $PLH_PREFIX (default: throwaway prefix /tmp/plh-entrees/pfx, created if missing).
# TAP output. Restores the cursor position.
set -u
# One injector at a time (CLAUDE.md, "Input test safety").
exec 8>/tmp/plh-input.lock
flock -w 120 8 || { echo "Bail out! lock /tmp/plh-input.lock unavailable"; exit 1; }
R=$(cd "$(dirname "$0")/../../.." && pwd); T=/tmp/plh-entrees; mkdir -p "$T"
B=$R/build
. "$R/runtime/env.sh"
export WINEPREFIX=${PLH_PREFIX:-$T/pfx} WINEDEBUG=-all PLH_BRIDGE_DIR=$T/br-$$
[ -f "$WINEPREFIX/system.reg" ] || timeout 180 "$WINE" wineboot -i >/dev/null 2>&1
n=0; fails=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fails=$((fails+1)); echo "not ok $n - $*"; }
orig=$("$R/src/input/tests/kwin-cursor-pos.sh")
printf 'workspace.cursorPosChanged.connect(function() { print("PLHMON " + workspace.cursorPos.x + " " + workspace.cursorPos.y); });\n' > "$T/mon.js"

FULL=0; [ "${1:-}" = --full ] && FULL=1
run() { # run <mode abs|rel> <injectprobe command ; command ; ...> -> KWin positions in $T/kwin.log
  local mode=$1; shift
  mkdir -p "$PLH_BRIDGE_DIR"; chmod 700 "$PLH_BRIDGE_DIR"
  PLH_INPUT_BACKEND=uinput PLH_INPUT_MOUSE_MODE=$mode "$B/bridge/plh-bridge" --runtime-dir "$PLH_BRIDGE_DIR" --capture-backend test > "$T/bridge.log" 2>&1 & local bp=$!
  for _ in $(seq 50); do [ -e "$PLH_BRIDGE_DIR/port" ] && break; sleep 0.1; done; sleep 1
  local since; since=$(date '+%Y-%m-%d %H:%M:%S')
  qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript plhmon >/dev/null 2>&1
  local id; id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$T/mon.js" plhmon)
  qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
  timeout 40 "$WINE" "$B/input/pe/plh-input-hook.exe" --seconds 8 > "$T/hook.log" 2>&1 & local hp=$!
  for _ in $(seq 100); do grep -q READY "$T/hook.log" 2>/dev/null && break; sleep 0.1; done
  : > "$T/inj.log"
  local IFS=';'; for cmd in $*; do
    IFS=' ' read -r -a a <<<"$cmd"
    if [ "${a[0]}" = GUARD ]; then guard || { echo "# guard: focus/position lost, stopping" >> "$T/inj.log"; GUARD_FAIL=1; break; }; continue; fi
    timeout 30 "$WINE" "$B/input/pe/injectprobe.exe" "${a[@]}" >> "$T/inj.log" 2>&1
  done
  sleep 0.5; qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript plhmon >/dev/null
  wait $hp; kill $bp 2>/dev/null; wait $bp 2>/dev/null
  journalctl --user --since "$since" -o cat | grep '^PLHMON' | sed 's/^PLHMON //' > "$T/kwin.log"
  tr -d '\r' < "$T/hook.log" | sed 's/^/# hook: /'
}

# KWin: outputs and expected translation (Windows puts the primary KWin output at 0,0)
mapfile -t outs < <(kscreen-doctor -o | sed 's/\x1b\[[0-9;]*m//g' | awk '/^Output:/{p=0} /priority 1$/{p=1} /Geometry:/{split($2,a,",");split($3,s,"x");print a[1],a[2],s[1],s[2],p}')
read -r px py < <(printf '%s\n' "${outs[@]}" | awk '$5==1{print $1,$2}')
targets=(); wargs=()
for o in "${outs[@]}"; do read -r x y w h _ <<<"$o"
  for t in "$((x+w/2)) $((y+h/2))" "$((x+137)) $((y+211))"; do targets+=("$t"); read -r tx ty <<<"$t"; wargs+=($((tx-px)) $((ty-py))); done
done
run abs "abs ${wargs[*]}"
grep -q "translation=+$px,+$py" "$T/hook.log" && ok "SET_LAYOUT: Windows->KWin translation +$px,+$py" || nok "translation"
for t in "${targets[@]}"; do grep -qx "$t" "$T/kwin.log" && ok "absolute SendInput -> KWin cursor exactly at $t" || nok "absolute $t not observed"; done

run rel "rel 10 0 10 -5 -20 5 100 0 0 50"
mapfile -t k < "$T/kwin.log"
deltas=""; for ((i=1; i<${#k[@]}; i++)); do read -r ax ay <<<"${k[i-1]}"; read -r bx by <<<"${k[i]}"; deltas+="$((bx-ax)),$((by-ay)) "; done
[ "$deltas" = "10,-5 -20,5 100,0 0,50 " ] && ok "relative SendInput -> exact KWin deltas ($deltas)" || nok "relative deltas: '$deltas' (expected 10,-5 -20,5 100,0 0,50 after the first)"

if [ $FULL = 1 ]; then
  d=$T; python3 "$R/src/input/tests/testwin.py" --timeout 40 > "$d/tw.log" 2>&1 & twpid=$!
  for _ in $(seq 60); do grep -q READY "$d/tw.log" 2>/dev/null && break; sleep 0.1; done; sleep 0.3
  read -r WX WY WW WH WACT < <("$R/src/input/tests/kwin-window-geom.sh" "PLH input test window")
  CX=$((WX + WW/2)); CY=$((WY + WH/2))
  guard() { [ "$(grep -E '^(READY|INACTIVE)$' "$d/tw.log" | tail -1)" = READY ] && [ "$("$R/src/input/tests/kwin-cursor-pos.sh")" = "$CX $CY" ]; }
  GUARD_FAIL=0
  run abs "abs $((CX-px)) $((CY-py));GUARD;click left;click right;click middle;wheel -120;hwheel 120;GUARD;key 0x87;sc 0x2a;sc 0x1d e0;legacy"
  kill $twpid 2>/dev/null; wait $twpid 2>/dev/null
  tw=$(cat "$d/tw.log"); echo "$tw" | sed 's/^/# testwin: /'
  if [ $GUARD_FAIL = 1 ] || [ "$WACT" != 1 ]; then nok "# SKIP clicks/keys: test window unfocused or no longer under the cursor"
  else
    chk() { if grep -q "$1" <<<"$tw"; then ok "$2"; else nok "$2"; fi; }
    chk "^BTN down 1 $((WW/2)) $((WH/2))" "SendInput left click received at the exact pixel (Wayland window)"
    chk "^BTN down 2 " "SendInput right click"
    chk "^BTN down 4 " "SendInput middle click"
    chk "^WHEEL -120 0" "SendInput wheel -120"
    chk "^WHEEL 0 -120\|^WHEEL 0 120" "SendInput horizontal wheel"
    [ "$(grep -c '^KEY down 194 ' <<<"$tw")" -ge 2 ] && ok "F24 via SendInput(VK) and keybd_event" || nok "F24 x2"
    chk "^KEY down 42 " "SendInput scancode 0x2A -> Left Shift"
    chk "^KEY down 97 " "SendInput scancode E0 1D -> Right Ctrl"
    chk "^KEY up 97 " "Right Ctrl release"
  fi
fi
"$B/input/plh-input-ctl" --settle 500 abs $orig >/dev/null 2>&1
rm -rf "$PLH_BRIDGE_DIR"
echo "1..$n"; exit $((fails != 0))
