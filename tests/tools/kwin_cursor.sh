#!/usr/bin/env bash
# REAL cursor position according to KWin (workspace.cursorPos), independent of Wine.
#   kwin_cursor.sh pos              → "X Y"
#   kwin_cursor.sh mon-start NAME    → starts a monitor (all positions), prints the start time
#   kwin_cursor.sh mon-stop NAME SINCE → stops and prints the observed "X Y" positions
# Same technique as src/input/tests/kwin-cursor-pos.sh (ephemeral KWin script + journal),
# rewritten here so the test does not depend on the code it judges.
set -eu
d=/tmp/plh-critique/kwin; mkdir -p "$d"
K() { qdbus6 org.kde.KWin "$@"; }
case "$1" in
pos)
  tag="PLHCRIT$$_$RANDOM"; since=$(date '+%Y-%m-%d %H:%M:%S')
  printf 'print("%s " + workspace.cursorPos.x + " " + workspace.cursorPos.y);\n' "$tag" >"$d/p$$.js"
  id=$(K /Scripting org.kde.kwin.Scripting.loadScript "$d/p$$.js" "plhcrit$$"); K "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
  out=""; for _ in $(seq 40); do out=$(journalctl --user --since "$since" -o cat 2>/dev/null | grep -m1 "$tag" || true); [ -n "$out" ] && break; sleep 0.05; done
  K /Scripting org.kde.kwin.Scripting.unloadScript "plhcrit$$" >/dev/null || true; rm -f "$d/p$$.js"
  [ -n "$out" ] || { echo "kwin_cursor: no response" >&2; exit 1; }
  echo "${out#*"$tag" }";;
mon-start)
  printf 'workspace.cursorPosChanged.connect(function(){ print("PLHCMON_%s " + workspace.cursorPos.x + " " + workspace.cursorPos.y); });\n' "$2" >"$d/m-$2.js"
  date '+%Y-%m-%d %H:%M:%S'
  K /Scripting org.kde.kwin.Scripting.unloadScript "plhcmon$2" >/dev/null 2>&1 || true
  id=$(K /Scripting org.kde.kwin.Scripting.loadScript "$d/m-$2.js" "plhcmon$2"); K "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null;;
mon-stop)
  sleep 0.3; K /Scripting org.kde.kwin.Scripting.unloadScript "plhcmon$2" >/dev/null || true; rm -f "$d/m-$2.js"
  journalctl --user --since "$3" -o cat 2>/dev/null | sed -n "s/^PLHCMON_$2 //p";;
esac
