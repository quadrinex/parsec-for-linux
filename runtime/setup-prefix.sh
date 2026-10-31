#!/usr/bin/env bash
# Creates / updates the dedicated Wine prefix runtime/prefix (idempotent).
#  - isolated wine-staging (Kron4ek binaries) in runtime/downloads/ (never /usr/bin/wine)
#  - DXVK installed in the prefix only (prefix DllOverrides)
#  - Windows 10, no winemenubuilder
# Usage: runtime/setup-prefix.sh [--no-dxvk]
set -euo pipefail
. "$(dirname "$0")/env.sh"

WITH_DXVK=1
[ "${1:-}" = "--no-dxvk" ] && WITH_DXVK=0

mkdir -p "$PLH_DL"

fetch() { # url sha256 dest
    local url=$1 sum=$2 dest=$3
    if [ -f "$dest" ] && echo "$sum  $dest" | sha256sum -c --status; then return 0; fi
    echo ">> downloading $url"
    curl -fsSL -o "$dest.part" "$url"
    echo "$sum  $dest.part" | sha256sum -c --status || { echo "!! invalid SHA256 sum for $url" >&2; rm -f "$dest.part"; exit 1; }
    mv "$dest.part" "$dest"
}

fetch "$WINE_STAGING_URL" "$WINE_STAGING_SHA256" "$PLH_DL/$WINE_STAGING_NAME.tar.xz"
[ -x "$PLH_WINE_DIR/bin/wine" ] || tar -C "$PLH_DL" -xf "$PLH_DL/$WINE_STAGING_NAME.tar.xz"
fetch "$DXVK_URL" "$DXVK_SHA256" "$PLH_DL/dxvk-$DXVK_VER.tar.gz"
[ -d "$PLH_DXVK_DIR/x64" ] || tar -C "$PLH_DL" -xzf "$PLH_DL/dxvk-$DXVK_VER.tar.gz"
[ -f "$PLH_DL/parsec-windows.exe" ] || curl -fsSL -o "$PLH_DL/parsec-windows.exe" "$PARSEC_URL"

# wine-plh: wine-staging + project fixes (ntdll), rebuilt only when the patches change
"$PLH_RUNTIME/build-wine-plh.sh"
. "$PLH_RUNTIME/env.sh"
echo ">> wine: $WINE $("$WINE" --version)  prefix: $WINEPREFIX"
case "$("$WINE" --version)" in *Staging*) ;; *) echo "!! this is not wine-staging" >&2; exit 1;; esac

# Create / update the prefix
"$WINE" wineboot -u >/dev/null 2>&1
"$WINESERVER" -w

reg_add() { "$WINE" reg add "$@" /f >/dev/null; }

# Windows 10
reg_add 'HKCU\Software\Wine' /v Version /t REG_SZ /d win10
# No file associations / menus in the host session
reg_add 'HKCU\Software\Wine\DllOverrides' /v winemenubuilder.exe /t REG_SZ /d ''

if [ "$WITH_DXVK" = 1 ]; then
    sys32="$WINEPREFIX/drive_c/windows/system32"
    syswow="$WINEPREFIX/drive_c/windows/syswow64"
    for d in d3d8 d3d9 d3d10core d3d11 dxgi; do
        install -m 644 "$PLH_DXVK_DIR/x64/$d.dll" "$sys32/$d.dll"
        install -m 644 "$PLH_DXVK_DIR/x32/$d.dll" "$syswow/$d.dll"
        reg_add 'HKCU\Software\Wine\DllOverrides' /v "$d" /t REG_SZ /d native
    done
    echo ">> DXVK $DXVK_VER installed in the prefix"
else
    for d in d3d8 d3d9 d3d10core d3d11 dxgi; do
        "$WINE" reg delete 'HKCU\Software\Wine\DllOverrides' /v "$d" /f >/dev/null 2>&1 || true
    done
    echo ">> DXVK overrides removed (Wine builtin DLLs)"
fi
"$WINESERVER" -w
echo ">> prefix ready"
