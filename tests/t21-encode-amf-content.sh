#!/usr/bin/env bash
# T21 — Encoding through the PE AMF API (amfrt64.dll, N3): the decoded CONTENT must match the input BGRA texture
# (R/G/B/W quadrants + moving black bar), not just be a valid stream. Also checks: strict decoding, leading IDR,
# no B-frames, submit->output latency, and CLEAN failure (code 3, no hang) if plh-bridge is absent.
# Starts its own private plh-bridge. Non-intrusive (nothing displayed).
. "$(dirname "$0")/lib/common.sh"
need_cmd ffmpeg ffprobe python3 x86_64-w64-mingw32-g++
need_prefix
dll="$REPO/build/encode/amf/amfrt64.dll"; [ -f "$dll" ] || fail_or_skip N3 "amfrt64.dll not built (make -C src/encode amf)"
cmp -s "$dll" "$PREFIX_DIR/drive_c/windows/system32/amfrt64.dll" || info "prefix amfrt64.dll ≠ build (rerun install-amf.sh?)"
out="$TMPD/amf"; rm -rf "$out"; mkdir -p "$out"
# Private bridge started/stopped by the test (the AMF client finds it through PLH_BRIDGE_DIR; test backend, no real capture).
trap stop_bridge EXIT
start_bridge "$TMPD/amf-rt" --capture-backend test
exe="$out/amf_content.exe"
x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -Wno-unused-parameter -Wno-overloaded-virtual -Wno-delete-non-virtual-dtor \
  -D_Static_assert=static_assert -static "$TESTS_DIR/pe/amf_content.cpp" -o "$exe" -ld3d11 || fail "compiling amf_content.cpp"
wine_env
errs=()
N=60
for codec in avc hevc; do
  f="$out/$codec.bin"
  (cd "$out" && PE_TIMEOUT=120 run_pe "$exe" "$codec" "Z:\\tmp\\plh-critique\\amf\\$codec.bin" $N) >"$out/$codec.log" 2>&1; rc=$?
  sed "s/^/  # [$codec] /" "$out/$codec.log" | grep -E "init=|lat_ms|RESULT|fail="
  [ $rc = 0 ] || { errs+=("$codec : client rc=$rc"); continue; }
  g=$(ffprobe -v error -select_streams v -show_frames -show_entries frame=width,height,key_frame,pict_type -of csv "$f" 2>/dev/null |
      awk -F, '{n++; if($2==1) k=k" "n-1; if($5=="B")b++; w=$3"x"$4} END{print n" "w" idr:"k" B:"b+0}')
  info "$codec : $g"
  case "$g" in "$N 1280x720 idr: 0 B:0"|"$N 1280x720 idr: 0"*" B:0") ;; *) errs+=("$codec : stream '$g'");; esac
  s=$(ffmpeg -v error -err_detect explode -i "$f" -f null - 2>&1 | head -1); [ -z "$s" ] || errs+=("$codec : strict decoding: $s")
  # Content: last frame decoded as RGB24
  ffmpeg -v error -y -i "$f" -vf "select=eq(n\,$((N-1)))" -frames:v 1 -f rawvideo -pix_fmt rgb24 "$out/$codec.rgb" 2>/dev/null
  c=$(python3 - "$out/$codec.rgb" $((N-1)) <<'P'
import sys
W,H=1280,720; d=open(sys.argv[1],'rb').read(); i=int(sys.argv[2])
if len(d)!=W*H*3: print("size",len(d)); sys.exit(1)
def px(x,y): o=(y*W+x)*3; return tuple(d[o:o+3])
exp={(320,180):(255,0,0),(960,180):(0,255,0),(320,360+180):(0,0,255),(960,540):(255,255,255),
     (i*10+20,700):(0,0,0),(i*10+120,700):None}
bad=[]
for (x,y),e in exp.items():
    p=px(x,y)
    if e is None: e=(0,0,255) if x<640 else (255,255,255)   # same band, outside the bar: quadrant color
    if max(abs(a-b) for a,b in zip(p,e))>40: bad.append(f"({x},{y}) {p} expected {e}")
print("OK" if not bad else "MISMATCH "+"; ".join(bad)); sys.exit(1 if bad else 0)
P
  ) || errs+=("$codec : content: $c")
  info "$codec content: $c"
  lat=$(sed -n 's/^lat_ms=//p' "$out/$codec.log" | tr -d '\r'); info "$codec submit->output latency (p50 max ms) : $lat"
  awk -v l="${lat%% *}" 'BEGIN{exit !(l+0>0 && l+0<50)}' || errs+=("$codec : latency p50 ${lat:-?} ms ≥ 50")
done
# Bridge absent: empty directory -> clean error, no hang or crash
mkdir -p "$out/nobridge"
(cd "$out" && PLH_BRIDGE_DIR="$out/nobridge" PE_TIMEOUT=60 run_pe "$exe" avc "Z:\\tmp\\plh-critique\\amf\\nb.bin" 5) >"$out/nb.log" 2>&1; rc=$?
info "bridge absent: rc=$rc $(tr '\n' ' ' <"$out/nb.log" | cut -c1-150)"
case $rc in 3) ;; 124) errs+=("bridge absent: hang (timeout)");; 0) errs+=("bridge absent: unexpected success");; *) errs+=("bridge absent: rc=$rc (crash?)");; esac
if [ ${#errs[@]} -gt 0 ]; then printf '  # - %s\n' "${errs[@]}"; fail_or_skip N3 "${#errs[@]} discrepancy(ies) : ${errs[0]}"; fi
pass "AMF PE: AVC+HEVC, BGRA content conforming, IDR, strict, latency, absent bridge handled"
