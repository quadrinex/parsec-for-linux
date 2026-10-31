#!/usr/bin/env bash
# Proof E5 (UTF-8 text, both directions) via plh-clipd.sh + plh-clipsync.exe. Saves/restores the KDE clipboard.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd); . "$R/runtime/env.sh"; T=/tmp/plh-eclaireur
K="qdbus6 org.kde.klipper /klipper"; saved=$($K getClipboardContents); trap '$K setClipboardContents "$saved" >/dev/null' EXIT
rm -rf "$XDG_RUNTIME_DIR/plh-bridge/clip"
x86_64-w64-mingw32-gcc -std=c11 -include string.h -o $T/plh-clipsync.exe "$R/src/clipboard/plh-clipsync.c"
x86_64-w64-mingw32-gcc -municode -o $T/probe.exe "$R/tests/pe/clipboard_probe.c"
WINEDEBUG=-all "$WINE" $T/plh-clipsync.exe "$XDG_RUNTIME_DIR/plh-bridge/clip" --seconds 25 >/dev/null 2>&1 & p1=$!
"$R/src/clipboard/plh-clipd.sh" 25 & p2=$!
sleep 4; fail=0
t1="plh-l2w-$RANDOM-é€"; $K setClipboardContents "$t1"; sleep 2
g=$(WINEDEBUG=-all "$WINE" $T/probe.exe get 2>/dev/null | sed -n 's/^text=//p' | tr -d '\r'); echo "L->W set=$t1 got=$g"; [ "$g" = "$t1" ] || fail=1
t2="plh-w2l-$RANDOM-ü"; WINEDEBUG=-all "$WINE" $T/probe.exe set "$t2" 6000 >/dev/null 2>&1 & p3=$!; sleep 3
g2=$($K getClipboardContents); echo "W->L set=$t2 got=$g2"; [ "$g2" = "$t2" ] || fail=1
wait $p3 $p1 $p2 2>/dev/null || true; ls -A "$XDG_RUNTIME_DIR/plh-bridge/clip" 2>/dev/null | wc -l | sed "s/^/fichiers restants: /"; stat -c "dir %a" "$XDG_RUNTIME_DIR/plh-bridge/clip"
[ $fail = 0 ] && echo "PASS E5" || { echo "FAIL E5"; exit 1; }
