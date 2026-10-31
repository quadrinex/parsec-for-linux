#!/usr/bin/env bash
# N3: amfrt64.dll + SimpleEncoder client under Wine (dedicated prefix); requires a running plh-bridge.
# Proves: strictly decodable stream (ffmpeg -err_detect explode), 90 frames, IDR at 0 and 30 (IDR forced by
# ForcePictureType), no B, 1280x720 — AVC/HEVC x NV12/BGRA.  Installs amfrt64.dll into the dedicated prefix.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
make -C "$ROOT/src/encode" amf >/dev/null || { echo "build failed"; exit 1; }
"$ROOT/src/encode/amf/install-amf.sh" >/dev/null
# shellcheck disable=SC1091
source "$ROOT/runtime/env.sh" >/dev/null 2>&1
T=/tmp/plh-encodeur/amftest; mkdir -p "$T"; cp "$ROOT/build/encode/amf/amf-simple.exe" "$T/"
n=0; fail=0
for c in "avc nv12" "avc bgra" "hevc nv12" "hevc bgra"; do
    set -- $c; n=$((n+1)); f=$T/$1-$2.bin; rm -f "$f"
    (cd "$T" && timeout 120 "$WINE" amf-simple.exe "$1" "$2" "Z:\\tmp\\plh-encodeur\\amftest\\$1-$2.bin" 90 >"$T/$1-$2.out" 2>&1)
    got=$(ffprobe -v error -select_streams v -show_frames -show_entries frame=width,height,key_frame,pict_type -of csv "$f" 2>/dev/null |
          awk -F, '{n++; if($2==1) k=k" "n-1; if($5=="B")b++; w=$3"x"$4} END{print n" "w" idr:"k" B:"b+0}')
    strict=$(ffmpeg -v error -err_detect explode -i "$f" -f null - 2>&1 | head -1)
    if [ "$got" = "90 1280x720 idr: 0 30 B:0" ] && [ -z "$strict" ]; then echo "ok $n - AMF $1 $2: $got"
    else echo "not ok $n - AMF $1 $2: '$got' strict='$strict'"; fail=$((fail+1)); fi
done
echo "1..$n"; exit $fail
