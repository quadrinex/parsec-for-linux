#!/usr/bin/env bash
# Frame-size tests of the encode path (virtual display / display switch). TAP-ish: "ok N - ..." / "not ok N - ...".
#   native : encbench -> libplhenc (VA-API), strict ffmpeg decode, decoded size check, H.264 limit refused by VA-API
#   amf    : amfrt64.dll under Wine against a PRIVATE plh-bridge (own runtime dir, port, binary name: the host is never
#            touched), sizes changed in one process: new component / Terminate+Init / ReInit(w,h); texture != encoder size.
# Usage: run-size-tests.sh [native|amf|all]     Needs: make -C src/encode all amf (and make -C src/bridge for amf).
# Not covered on purpose: odd sizes in D3D11 NV12 textures (invalid in D3D11; DXVK device-lost, GPU reset seen 2026-11-08).
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
B="$ROOT/build/encode/encbench"; T=/tmp/plh-encodeur/sizes; mkdir -p "$T"; n=0; fail=0
ok() { n=$((n+1)); if [ "$1" = 0 ]; then echo "ok $n - $2"; else echo "not ok $n - $2"; fail=$((fail+1)); fi; }
strict() { ffmpeg -v error -err_detect explode -i "$1" -f null - 2>&1 | head -1; }
dims() { ffprobe -v error -select_streams v -show_entries stream=width,height -of csv=p=0 "$1" 2>/dev/null | head -1; }
mode=${1:-all}

if [ "$mode" = native ] || [ "$mode" = all ]; then
    for c in h264 hevc; do
        for s in 1280x720 1366x768 1600x900 1920x1080 2560x1080 2560x1440 3440x1440 3840x2160 4096x2304 1000x600 640x360; do
            f=$T/$c-$s.bin; rm -f "$f"
            "$B" --codec $c --size $s --frames 15 --rc cbr --bitrate 20000000 --src cpu --out "$f" >/dev/null 2>&1
            w=${s%x*}; h=${s#*x}
            [ "$(dims "$f")" = "$w,$h" ] && [ -z "$(strict "$f")" ]; ok $? "native $c $s decodes strictly at exactly $s"
        done
        # odd sizes: 4:2:0 cropping is in units of 2 px, so the stream is rounded UP to even
        f=$T/$c-odd.bin; "$B" --codec $c --size 1367x769 --frames 15 --rc cbr --bitrate 8000000 --src cpu --out "$f" >/dev/null 2>&1
        [ "$(dims "$f")" = "1368,770" ] && [ -z "$(strict "$f")" ]; ok $? "native $c 1367x769 -> stream 1368x770 (odd sizes round up to even)"
    done
    for s in 4112x2160 5120x2880 7680x4320; do
        out=$("$B" --backend vaapi --codec h264 --size $s --frames 2 --src cpu 2>&1)
        echo "$out" | grep -q "exceeds the driver maximum"; ok $? "native h264 $s refused by VA-API (limit 4096x4096)"
    done
    for s in 4112x2160 5120x2880 7680x4320; do
        f=$T/hevc-$s.bin; rm -f "$f"; "$B" --backend vaapi --codec hevc --size $s --frames 6 --rc cbr --bitrate 30000000 --src cpu --out "$f" >/dev/null 2>&1
        [ "$(dims "$f")" = "${s%x*},${s#*x}" ] && [ -z "$(strict "$f")" ]; ok $? "native hevc $s on VA-API (limit 8192x4352)"
    done
    # size change between sessions in one process: covered by test_api / amf below (each session is a new plh_enc_create)
fi

if [ "$mode" = amf ] || [ "$mode" = all ]; then
    # shellcheck disable=SC1091
    source "$ROOT/runtime/env.sh" >/dev/null 2>&1
    R=$T/run; BR="$T/bin/plh-encsizes-bridge"; mkdir -p "$T/bin"; cp "$ROOT/build/bridge/plh-bridge" "$BR"
    rm -rf "$R"; mkdir -m 700 "$R"
    setsid "$BR" --port $((40000 + RANDOM % 20000)) --runtime-dir "$R" --capture-backend test >"$T/bridge.log" 2>&1 </dev/null & BP=$!
    trap 'kill $BP 2>/dev/null' EXIT
    for _ in 1 2 3 4 5 6 7 8 9 10; do grep -q "ready" "$T/bridge.log" 2>/dev/null && break; sleep 0.5; done
    W=$T/wine; mkdir -p "$W"; cp "$ROOT/build/encode/amf/"{amf-sizes.exe,amfrt64.dll} "$W/"   # private copy: the prefix's DLL is not touched
    export PLH_BRIDGE_DIR="$R" PLH_AMF_LOG="Z:\\tmp\\plh-encodeur\\sizes\\amf.log"
    # One attempt = one wine process + checks of every size. Another teammate's test can kill wineserver/wine under
    # the shared prefix mid-run (seen: truncated streams, random subsets failing): retry once, report only the 2nd result.
    attempt() { # codec fmt mode sizes... ; prints result lines to $W/res.txt, returns number of failures
        local c=$1 fm=$2 md=$3; shift 3; rm -f "$W"/o-*.bin "$W/res.txt"
        (cd "$W" && timeout 300 "$WINE" amf-sizes.exe $c $fm 'Z:\tmp\plh-encodeur\sizes\wine\o' $md "$@" >"$W/out.txt" 2>&1)
        local i=0 s bad=0
        for s in "$@"; do
            local sz=${s%%:*} f=$W/o-$i.bin
            local k; k=$(ffprobe -v error -show_frames -select_streams v -show_entries frame=key_frame -of csv=p=0 "$f" 2>/dev/null | awk '{n++; if($1==1)k=k" "n-1} END{print n" frames, idr:"k}')
            if [ "$(dims "$f")" = "${sz%x*},${sz#*x}" ] && [ -z "$(strict "$f")" ] &&
               { [ "$k" = "12 frames, idr: 0 6" ] || { [ $c = avc ] && [ "${sz%x*}" -gt 4096 ] && [ "$k" = "12 frames, idr: 6" ]; }; }   # x264 fallback: ffprobe does not flag frame 0 (SEI first)
            then echo "0 amf $c $fm $md size $s: decodes strictly, ${sz}, $k" >>"$W/res.txt"
            else echo "1 amf $c $fm $md size $s: decodes strictly, ${sz}, $k" >>"$W/res.txt"; bad=$((bad+1)); fi
            i=$((i+1))
        done
        return $bad
    }
    run() { attempt "$@" || { echo "# retrying $* (previous attempt failed)"; attempt "$@"; }
            while read -r st rest; do ok "$st" "$rest"; done <"$W/res.txt"; }
    S="1920x1080 1280x720 2560x1440 3840x2160 1366x768 4096x2160 1000x600 1600x900 1920x1080"
    for c in avc hevc; do for fm in nv12 bgra; do for md in new reinit reinit2; do run $c $fm $md $S; done; done; done
    for c in avc hevc; do for fm in nv12 bgra; do run $c $fm new 1920x1080:1280x720 1280x720:1920x1080 1366x768:1920x1080 1920x1080; done; done
    run avc bgra new 4112x2160 1920x1080 2>/dev/null   # >4096: VA-API refuses, the bridge falls back to software x264 (no failure)
    run hevc bgra new 7680x4320 1280x720
fi
echo "1..$n"; exit $fail
