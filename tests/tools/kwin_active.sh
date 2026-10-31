#!/bin/bash
# kwin_active.sh -> caption of the ACTIVE KWin window (KWin script + journal). Empty if unknown.
set -u
d=/tmp/plh-critique; mkdir -p $d; tag="PLHACT$$_$RANDOM"; name="plhact$$"
printf 'print("%s " + (workspace.activeWindow ? workspace.activeWindow.caption : "") + " END");\n' "$tag" > $d/act-$$.js
since=$(date '+%Y-%m-%d %H:%M:%S')
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript $d/act-$$.js "$name")
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
out=""; for _ in $(seq 40); do out=$(journalctl --user --since "$since" -o cat | grep "$tag" | tail -1 || true); [ -n "$out" ] && break; sleep 0.05; done
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$name" >/dev/null 2>&1; rm -f $d/act-$$.js
echo "$out" | sed "s/^$tag //; s/ END\$//"
