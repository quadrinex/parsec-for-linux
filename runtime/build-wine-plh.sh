#!/usr/bin/env bash
# Builds runtime/downloads/wine-plh-<ver>: a copy of the isolated Kron4ek wine-staging where only the
# PE modules patched by the project (runtime/patches/wine/*.patch) are replaced.
# Rebuilt modules: PLH_WINE_MODULES (x86_64 + i386). See runtime/patches/wine/ and docs/api-map.md.
PLH_WINE_MODULES="ntdll wtsapi32 winhttp user32"
# Unix part (.so) rebuilt: ntdll.so (patch 0004, convert_control_headers).
PLH_WINE_UNIX="ntdll"
# Touches neither /usr/bin/wine nor anything outside runtime/downloads and /tmp/plh-eclaireur.
# Idempotent: does nothing if the fingerprint (version + patches) is unchanged.
set -euo pipefail
. "$(dirname "$0")/env.sh"

WORK=/tmp/plh-eclaireur/winebuild
SRC_URL="https://dl.winehq.org/wine/source/11.x/wine-${WINE_STAGING_VER}.tar.xz"
STG_URL="https://github.com/wine-staging/wine-staging/archive/refs/tags/v${WINE_STAGING_VER}.tar.gz"
PATCHES=("$PLH_RUNTIME"/patches/wine/*.patch)
STAMP="$( (cat "${PATCHES[@]}"; echo "$PLH_WINE_MODULES $PLH_WINE_UNIX") | sha256sum | cut -c1-16)-$WINE_STAGING_VER"

if [ -f "$PLH_WINE_PLH_DIR/.plh-stamp" ] && [ "$(cat "$PLH_WINE_PLH_DIR/.plh-stamp")" = "$STAMP" ]; then
    echo ">> wine-plh up to date ($STAMP)"; exit 0
fi
[ -x "$PLH_WINE_DIR/bin/wine" ] || { echo "run setup-prefix.sh first (base wine-staging)" >&2; exit 1; }

mkdir -p "$WORK" && cd "$WORK"
[ -f "wine-$WINE_STAGING_VER.tar.xz" ] || curl -fsSL -o "wine-$WINE_STAGING_VER.tar.xz" "$SRC_URL"
[ -f staging.tar.gz ] || curl -fsSL -o staging.tar.gz "$STG_URL"
rm -rf "wine-$WINE_STAGING_VER" "wine-staging-$WINE_STAGING_VER" build64
tar xf "wine-$WINE_STAGING_VER.tar.xz"
tar xzf staging.tar.gz
cd "wine-$WINE_STAGING_VER"
python3 "../wine-staging-$WINE_STAGING_VER/staging/patchinstall.py" --all -d . >/dev/null
for p in "${PATCHES[@]}"; do echo ">> patch $(basename "$p")"; patch -p1 --no-backup-if-mismatch < "$p"; done
mkdir -p ../build64 && cd ../build64
"../wine-$WINE_STAGING_VER/configure" --enable-archs=x86_64,i386 --without-x --without-freetype \
    --without-wayland --without-gstreamer --without-vulkan --without-opencl --without-cups --without-sane \
    --without-gphoto --without-pcap --without-usb --without-v4l2 --without-krb5 --without-netapi \
    --without-gnutls --without-pulse --without-alsa --without-oss --without-ffmpeg --without-capi \
    --without-dbus --without-sdl --without-udev --without-inotify --without-xinerama --without-opengl \
    --without-fontconfig --without-gssapi --without-pcsclite >configure.log 2>&1
targets=()
for m in $PLH_WINE_MODULES; do targets+=("dlls/$m/x86_64-windows/$m.dll" "dlls/$m/i386-windows/$m.dll"); done
for m in $PLH_WINE_UNIX; do targets+=("dlls/$m/$m.so"); done
make -j"$(nproc)" "${targets[@]}" >make.log 2>&1

rm -rf "$PLH_WINE_PLH_DIR.tmp"
cp -a "$PLH_WINE_DIR" "$PLH_WINE_PLH_DIR.tmp"
for m in $PLH_WINE_MODULES; do
    x86_64-w64-mingw32-strip -o "$PLH_WINE_PLH_DIR.tmp/lib/wine/x86_64-windows/$m.dll" "dlls/$m/x86_64-windows/$m.dll"
    i686-w64-mingw32-strip -o "$PLH_WINE_PLH_DIR.tmp/lib/wine/i386-windows/$m.dll" "dlls/$m/i386-windows/$m.dll"
done
for m in $PLH_WINE_UNIX; do
    strip -o "$PLH_WINE_PLH_DIR.tmp/lib/wine/x86_64-unix/$m.so" "dlls/$m/$m.so"
done
echo "$STAMP" > "$PLH_WINE_PLH_DIR.tmp/.plh-stamp"
rm -rf "$PLH_WINE_PLH_DIR"
mv "$PLH_WINE_PLH_DIR.tmp" "$PLH_WINE_PLH_DIR"
echo ">> wine-plh ready : $PLH_WINE_PLH_DIR ($STAMP)"
