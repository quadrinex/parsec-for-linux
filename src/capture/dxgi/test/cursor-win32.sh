#!/usr/bin/env bash
# Cursor test (owner: capture). Parsec imports GetCursorInfo/GetIconInfo/GetCursorPos; the DXGI pointer path is not
# the only way it can learn the host cursor. This checks, in a THROWAWAY prefix (never runtime/prefix, no injection):
#   1. GetCursorInfo reports a non-NULL hCursor with CURSOR_SHOWING;
#   2. GetIconInfo(hCursor) succeeds (a cursor image is retrievable).
# Verdict today: FAIL (hCursor NULL / GetIconInfo fails): Wine never sees the KWin cursor. Exit 1 on failure.
set -u
R=$(cd "$(dirname "$0")/../../../.." && pwd); T=/tmp/plh-capture/cursor-win32; mkdir -p "$T"
x86_64-w64-mingw32-gcc -std=gnu11 -Wall -Wextra -o "$T/cursor_probe.exe" "$R/src/capture/dxgi/test/cursor_probe.c" -luser32 -lgdi32 || exit 2
. "$R/runtime/env.sh"; export WINEPREFIX=$T/pfx WINEDEBUG=-all
[ -d "$WINEPREFIX" ] || timeout 120 "$WINE" wineboot -i >/dev/null 2>&1
out=$(timeout 30 "$WINE" "$T/cursor_probe.exe" 3 2>&1 | tr -d '\r'); echo "$out" | sed -n '1p;$p' | sed 's/^/  # /'
echo "$out" | grep -q 'info_ok=1 flags=0x1 hcur=0*[1-9a-f][0-9a-f]* .*iconinfo_ok=1' && { echo "PASS cursor visible through GetCursorInfo/GetIconInfo"; exit 0; }
echo "FAIL Wine GetCursorInfo/GetIconInfo give no cursor image (hCursor NULL or GetIconInfo fails)"; exit 1
