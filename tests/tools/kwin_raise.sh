#!/bin/bash
# kwin_raise.sh PREFIX: brings to the foreground (keepAbove + raise) the KWin windows whose caption starts with PREFIX,
# so the user's desktop windows do not hide the test pattern during the capture tests.
d=/tmp/plh-critique; mkdir -p $d; name="plhraise$$"
cat > $d/raise-$$.js <<JS
var l = workspace.windowList ? workspace.windowList() : workspace.clientList();
for (var i = 0; i < l.length; i++) if (String(l[i].caption).indexOf("$1") == 0) { l[i].keepAbove = true; workspace.raiseWindow(l[i]); }
JS
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript $d/raise-$$.js "$name")
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null; sleep 0.3
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$name" >/dev/null 2>&1; rm -f $d/raise-$$.js
