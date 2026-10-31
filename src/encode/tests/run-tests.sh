#!/usr/bin/env bash
# Tests of the native encoder (N1/N2). TAP output. Usage: run-tests.sh <build/encode directory>
# Checks independent of our code: ffprobe/ffmpeg (strict decoding, picture
# types, PSNR against the source pattern).
set -u
BUILD=${1:-$(dirname "$0")/../../../build/encode}
B=$BUILD/encbench
T=/tmp/plh-encodeur/tests.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
n=0; fail=0
ok()   { n=$((n+1)); echo "ok $n - $*"; }
nok()  { n=$((n+1)); fail=$((fail+1)); echo "not ok $n - $*"; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else nok "$d"; fi; }

# expected key frames / no B / frame count
frames_ok() { # file expected_count "idr,list"
    local f=$1 want=$2 keys=$3 got
    got=$(ffprobe -v error -select_streams v -show_frames -show_entries frame=pict_type,key_frame -of csv=p=0 "$f" |
          awk -F, '{n++; if($1==1) k=k (k?",":"") n-1; if($2=="B") b++} END{printf "%d %s %d", n, k, b+0}')
    [ "$got" = "$want $keys 0" ] || { echo "#   $f : obtenu '$got', attendu '$want $keys 0'"; return 1; }
}
strict_decode() { [ -z "$(ffmpeg -v error -err_detect explode -i "$1" -f null - 2>&1)" ]; }
psnr_min() { # source.bgrx stream WxH -> minimum mean PSNR per frame
    ffmpeg -v error -i "$1" -f rawvideo -pix_fmt bgr0 -s "$3" -i "$2" -lavfi \
      "[1:v]settb=1/60,setpts=N,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p[r];[0:v]settb=1/60,setpts=N,format=yuv420p[d];[d][r]psnr=stats_file=$T/psnr.log" -f null - >/dev/null 2>&1
    awk '{split($6,a,":"); v=a[2]+0; if(NR==1||v<m)m=v} END{print m+0}' "$T/psnr.log"
}

echo "TAP version 13"
[ -x "$B" ] || { echo "1..1"; echo "not ok 1 - $B absent (make -C src/encode)"; exit 1; }

# 1. conformance + IDR on demand + fidelity (PSNR) for each codec x source
for c in h264 hevc; do
  for s in bgrx nv12 cpu; do
    f=$T/$c-$s.bin
    if "$B" --codec $c --src $s --modifier auto --frames 20 --rc cqp --qp 22 --idr-at 7,13 \
         --out "$f" --dump-src "$T/src.bgrx" >"$T/log" 2>&1; then
      check "$c/$s: strict decode without errors" strict_decode "$f"
      check "$c/$s: 20 frames, IDR exactly at 0,7,13, no B" frames_ok "$f" 20 "0,7,13"
      p=$(psnr_min "$f" "$T/src.bgrx" 1920x1080)
      check "$c/$s: min PSNR per frame $p dB >= 38 (right picture, right order)" awk -v p="$p" 'BEGIN{exit !(p>=38)}'
    else
      nok "$c/$s : encodage"; sed 's/^/#   /' "$T/log"
    fi
    rm -f "$T/src.bgrx"
  done
done

# 2. H.264 profiles and sizes
for pr in cb:"Constrained Baseline" main:Main high:High; do
  k=${pr%%:*}; want=${pr#*:}
  "$B" --codec h264 --profile $k --frames 30 --out "$T/p.h264" >/dev/null 2>&1
  got=$(ffprobe -v error -show_entries stream=profile,width,height -of csv=p=0 "$T/p.h264")
  check "h264 profil $k : '$got'" test "$got" = "$want,1920,1080"
done

# 3. CBR / VBR bitrate within +-10 %
for rc in cbr vbr; do
  out=$("$B" --codec h264 --rc $rc --bitrate 10000000 --peak 15000000 --frames 300 --out "$T/r.h264")
  br=$(sed -n 's/^mean_bitrate=\([0-9.]*\).*/\1/p' <<<"$out")
  check "h264 $rc: mean bitrate $br Mbit/s within 10 +-10 %" awk -v b="$br" 'BEGIN{exit !(b>=9 && b<=11)}'
done

# 4. limits: H.264 5760x1080 cleanly refused (driver max 4096), HEVC accepted
"$B" --codec h264 --size 5760x1080 --frames 2 >"$T/log" 2>&1; rc=$?
check "h264 5760x1080 refused (code 2, clear message)" test $rc = 2 -a -n "$(grep -o 'exceeds the driver maximum' "$T/log")"
"$B" --codec hevc --size 5760x1080 --frames 30 --idr-at 15 --out "$T/w.hevc" >/dev/null 2>&1
got=$(ffprobe -v error -show_entries stream=width,height -of csv=p=0 "$T/w.hevc")
check "hevc 5760x1080: size '$got'" test "$got" = "5760,1080"
check "hevc 5760x1080: strict decode + IDR 0,15" eval 'strict_decode "$T/w.hevc" && frames_ok "$T/w.hevc" 30 "0,15"'

# 5. latency (real 60 fps cadence) — loose thresholds, the numbers are displayed
for cfg in "h264 1920x1080 8" "hevc 1920x1080 8" "hevc 5760x1080 20"; do
  set -- $cfg
  out=$("$B" --codec $1 --size $2 --src bgrx --modifier auto --frames 300 --realtime --bitrate 30000000)
  echo "# $1 $2 : $(sed -n 2p <<<"$out")"
  p95=$(sed -n 's/.* p95=\([0-9.]*\).*/\1/p' <<<"$out")
  check "$1 $2 : p95 latency $p95 ms < $3 ms" awk -v p="$p95" -v s="$3" 'BEGIN{exit !(p<s)}'
done

# 6. bridge encode service: native client (unix), native TCP+token, PE under Wine (TCP + shm Z:)
BR=$BUILD/../bridge/plh-bridge
if [ -x "$BR" ] && [ -z "${PLH_SKIP_BRIDGE:-}" ]; then
  RD=$T/rt; mkdir -m 700 "$RD"
  "$BR" --runtime-dir "$RD" --capture-backend test >"$T/bridge.log" 2>&1 & BP=$!
  for _ in $(seq 50); do [ -s "$RD/port" ] && [ -S "$RD/bridge.sock" ] && break; sleep 0.1; done
  export PLH_BRIDGE_DIR=$RD
  bridge_case() { # name file expected_keys command...
    local name=$1 f=$2 keys=$3; shift 3
    if "$@" >"$T/c.log" 2>&1 && grep -q "^RESULT.*keys=$keys .*close=0" "$T/c.log"; then
      check "bridge $name: strict decode" strict_decode "$f"
      local nf; nf=$(grep -o 'frames=[0-9]*' "$T/c.log" | cut -d= -f2)
      local kk; kk=$(grep -o 'keys=[0-9,]*' "$T/c.log" | cut -d= -f2)
      check "bridge $name: $nf frames, IDR at $kk, no B" frames_ok "$f" "$nf" "$kk"
      echo "# $name : $(grep '^RESULT' "$T/c.log")"
    else nok "pont $name"; sed 's/^/#   /' "$T/c.log"; fi
  }
  bridge_case "natif unix h264 bgra" "$T/b1.h264" "0,30,90" "$BUILD/plh-enctest" -c h264 -n 120 -i 30,90 -r -o "$T/b1.h264"
  bridge_case "natif unix hevc nv12" "$T/b2.hevc" "0,20" "$BUILD/plh-enctest" -c hevc -f nv12 -n 60 -i 20 -o "$T/b2.hevc"
  bridge_case "natif tcp h264" "$T/b3.h264" "0" env PLH_BRIDGE_FORCE_TCP=1 "$BUILD/plh-enctest" -c h264 -n 30 -o "$T/b3.h264"
  if command -v wine >/dev/null; then
    export WINEPREFIX=${PLH_TEST_WINEPREFIX:-/tmp/plh-encodeur/wpfx} WINEDEBUG=-all
    [ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1
    bridge_case "PE sous Wine h264" "$T/b4.h264" "0,30,90" timeout 120 wine "$BUILD/plh-enctest.exe" -c h264 -n 120 -i 30,90 -o "Z:$T/b4.h264"
    grep -q 'connected (tcp)' "$T/c.log" && ok "PE under Wine: TCP transport" || nok "PE under Wine: TCP transport"
  fi
  # bitrate reconfigured on the fly: the 2nd half of stream b1 (bitrate /4) must be much smaller
  half=$(ffprobe -v error -show_entries packet=size -of csv=p=0 "$T/b1.h264" | awk 'NR>1&&NR<=60{a+=$1} NR>61{b+=$1} END{printf "%.2f", b/a}')
  check "bitrate reconfig /4 mid-stream: size ratio 2nd/1st half $half < 0.5" awk -v r="$half" 'BEGIN{exit !(r<0.5)}'
  kill $BP; wait $BP 2>/dev/null
fi

# 7. software x264 backend (last resort): conformance, IDR, PSNR, bitrate
for src in cpu; do
  f=$T/sw.h264
  if "$B" --backend software --codec h264 --src $src --frames 20 --rc cqp --qp 22 --idr-at 7,13 \
       --out "$f" --dump-src "$T/src.bgrx" >"$T/log" 2>&1; then
    check "software/h264: strict decode without errors" strict_decode "$f"
    check "software/h264: 20 frames, IDR exactly at 0,7,13, no B" frames_ok "$f" 20 "0,7,13"
    p=$(psnr_min "$f" "$T/src.bgrx" 1920x1080)
    check "software/h264 : PSNR min $p dB >= 38" awk -v p="$p" 'BEGIN{exit !(p>=38)}'
  else nok "software/h264 : encodage"; sed 's/^/#   /' "$T/log"; fi
  rm -f "$T/src.bgrx"
done
if "$B" --backend software --codec h264 --src cpu --frames 60 --rc cbr --bitrate 4000000 --size 1280x720 \
     --out "$T/swc.h264" >"$T/log" 2>&1; then
  sz=$(stat -c %s "$T/swc.h264")
  check "software/h264 CBR 4 Mbit/s: actual rate $((sz*8)) bit over 1 s <= 5 Mbit/s" test $((sz*8)) -le 5000000
else nok "software CBR"; sed 's/^/#   /' "$T/log"; fi
"$B" --backend software --codec hevc --src cpu --frames 2 >"$T/log" 2>&1 && nok "software/hevc must be refused" || ok "software/hevc cleanly refused"

echo "1..$n"
[ $fail = 0 ]
