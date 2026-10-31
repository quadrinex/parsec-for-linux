#!/bin/bash
# Prints "X Y": real cursor position according to KWin (workspace.cursorPos), via a temporary
# KWin script whose print() output is read from the user journal.
set -eu
d=/tmp/plh-entrees; mkdir -p "$d"
tag="PLHPOS$$_$RANDOM"
printf 'print("%s " + workspace.cursorPos.x + " " + workspace.cursorPos.y);\n' "$tag" > "$d/pos-$$.js"
name="plhpos$$"
since=$(date '+%Y-%m-%d %H:%M:%S')
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$d/pos-$$.js" "$name")
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
out=""
for _ in $(seq 20); do
    out=$(journalctl --user --since "$since" -o cat 2>/dev/null | grep -m1 "$tag" || true)
    [ -n "$out" ] && break
    sleep 0.05
done
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$name" >/dev/null || true
rm -f "$d/pos-$$.js"
[ -n "$out" ] || { echo "kwin-cursor-pos: no response" >&2; exit 1; }
echo "${out#*"$tag" }"
