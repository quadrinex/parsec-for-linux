#!/usr/bin/env bash
# Proof for patch 0006: without HKLM\Software\Wine\PlhVirtualDisplay the answers are Wine's own; with the key
# (Active=0) the placeholder appears as one more adapter/monitor with exactly one mode; Active=1 steps aside.
# No client needed. Uses a throwaway prefix (/tmp), so the live prefix and a running host are never touched.
set -euo pipefail; D=$(cd "$(dirname "$0")" && pwd); . "$D/../../env.sh"; T=/tmp/plh-eclaireur/vdrepro; mkdir -p $T
x86_64-w64-mingw32-gcc -Wall -Wextra -o $T/vdrepro.exe "$D/vdrepro.c"
export WINEPREFIX=/tmp/plh-eclaireur/vdpfx WINEDEBUG=-all WINEDLLOVERRIDES=winemenubuilder.exe=d
K='HKLM\Software\Wine\PlhVirtualDisplay'
run() { "$WINE" $T/vdrepro.exe 2>&1 | sed 's/\r$//'; }
trap '"$WINESERVER" -k 2>/dev/null || true' EXIT
"$WINE" reg delete "$K" /f >/dev/null 2>&1 || true
run > $T/none.txt
"$WINE" reg add "$K" /v DeviceName /t REG_SZ /d '\\.\DISPLAYX' /f >/dev/null
for kv in Left=5760 Top=0 Width=1920 Height=1080 Active=0; do
    "$WINE" reg add "$K" /v "${kv%%=*}" /t REG_DWORD /d "${kv#*=}" /f >/dev/null; done
run > $T/placeholder.txt
"$WINE" reg add "$K" /v Active /t REG_DWORD /d 1 /f >/dev/null
run > $T/active.txt
echo "== no key =="; cat $T/none.txt; echo "== key, Active=0 =="; diff $T/none.txt $T/placeholder.txt || true
echo "== key, Active=1 =="; diff $T/none.txt $T/active.txt && echo "identical to no key"
n=$(diff $T/none.txt $T/placeholder.txt | grep -c '^>' || true)
grep -q 'DISPLAYX' $T/placeholder.txt && ! grep -q 'DISPLAYX' $T/none.txt && cmp -s $T/none.txt $T/active.txt \
    && grep -c 'mode\[' $T/placeholder.txt >/dev/null && echo "PASS vdrepro ($n added lines)"
