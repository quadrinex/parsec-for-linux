#!/bin/bash
# kwin-window-geom.sh "Title" -> "X Y W H ACTIVE(0/1)" of the window (client geometry, desktop coordinates).
set -eu
title="$1"; d=/tmp/plh-entrees; mkdir -p "$d"; tag="PLHWIN$$_$RANDOM"; name="plhwin$$"
cat > "$d/win-$$.js" <<JS
var ws = workspace.windowList ? workspace.windowList() : workspace.clientList();
for (var i = 0; i < ws.length; i++) { var c = ws[i];
  if (c.caption == "$title") { var g = c.clientGeometry;
    print("$tag " + Math.round(g.x) + " " + Math.round(g.y) + " " + Math.round(g.width) + " " + Math.round(g.height) + " " + (workspace.activeWindow == c ? 1 : 0)); } }
print("$tag END");
JS
since=$(date '+%Y-%m-%d %H:%M:%S')
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$d/win-$$.js" "$name")
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
out=""
for _ in $(seq 20); do out=$(journalctl --user --since "$since" -o cat | grep "$tag" || true); grep -q END <<<"$out" && break; sleep 0.05; done
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$name" >/dev/null || true
rm -f "$d/win-$$.js"
grep -v END <<<"$out" | head -1 | sed "s/^$tag //"
