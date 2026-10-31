#!/usr/bin/env bash
# Installs/removes the dxgi.dll proxy in the DEDICATED Wine prefix only.
#   install-proxy.sh        : system32/dxgi.dll (DXVK) -> dxgi_dxvk.dll ; proxy -> dxgi.dll
#   install-proxy.sh --undo : restores DXVK's dxgi.dll
# Idempotent. Never touches the system Wine or ~/.wine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/runtime/env.sh"
S="$WINEPREFIX/drive_c/windows/system32"
P="$ROOT/build/dxgi/dxgi.dll"
[ -d "$S" ] || { echo "dedicated prefix missing" >&2; exit 1; }
if [ "${1:-}" = --undo ]; then
    [ -f "$S/dxgi_dxvk.dll" ] && mv -f "$S/dxgi_dxvk.dll" "$S/dxgi.dll"
    echo "DXVK dxgi.dll restored"; exit 0
fi
[ -f "$P" ] || { echo "proxy not built: make -C src/capture/dxgi" >&2; exit 1; }
if [ ! -f "$S/dxgi_dxvk.dll" ]; then
    # does not overwrite dxgi_dxvk.dll: the current dxgi.dll must be DXVK's (large)
    [ "$(stat -c %s "$S/dxgi.dll")" -gt 1000000 ] || { echo "the prefix dxgi.dll is not DXVK" >&2; exit 1; }
    mv "$S/dxgi.dll" "$S/dxgi_dxvk.dll"
fi
cp -f "$P" "$S/dxgi.dll"
echo "proxy installed in $S"
