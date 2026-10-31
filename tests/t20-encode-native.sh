#!/usr/bin/env bash
# N1 / T3 (native part) — the encoder's VA-API bench (build/encode/encbench),
# checked independently by ffprobe/ffmpeg for H.264 and HEVC:
#   - 300 frames decoded without error, 1920x1080;
#   - no B-frames; key frames exactly at 0, 100, 250 (IDR on demand);
#   - mean bitrate within ±10 % of the CBR target;
#   - p99 latency < 16.6 ms (one frame at 60 Hz), values reported.
# The "via the PE encoding API" (AMF) part is t21.
. "$(dirname "$0")/lib/common.sh"
need_cmd ffprobe ffmpeg
B="$REPO/build/encode/encbench"
[ -x "$B" ] || fail_or_skip N1 "encbench not built ($B)"
out="$TMPD/enc"; mkdir -p "$out"
errs=()
for codec in h264 hevc; do
  f="$out/t20.$codec"; rm -f "$f"
  log=$(timeout 120 "$B" --codec $codec --size 1920x1080 --frames 300 --rc cbr --bitrate 20000000 --idr-at 100,250 --out "$f" 2>&1); rc=$?
  echo "$log" | sed "s/^/  # [$codec] /"
  [ $rc = 0 ] && [ -s "$f" ] || { errs+=("$codec : encbench rc=$rc"); continue; }
  st=$(ffprobe -v error -show_entries stream=codec_name,width,height,has_b_frames,profile -of compact=p=0 "$f")
  info "[$codec] $st"
  echo "$st" | grep -q 'width=1920|height=1080' || errs+=("$codec : size ≠ 1920x1080")
  fr=$(ffprobe -v error -select_streams v -show_entries frame=key_frame,pict_type -of csv=p=0 "$f")
  n=$(echo "$fr" | grep -c .)
  keys=$(echo "$fr" | awk -F, '{if($1==1) printf "%d ", NR-1}')
  nb=$(echo "$fr" | grep -c ',B')
  info "[$codec] frames=$n keys=[$keys] B=$nb"
  [ "$n" = 300 ] || errs+=("$codec : $n frames decoded instead of 300")
  [ "$keys" = "0 100 250 " ] || errs+=("$codec : key frames [$keys] instead of [0 100 250]")
  [ "$nb" = 0 ] || errs+=("$codec : $nb B-frames")
  derr=$(ffmpeg -v error -i "$f" -f null - 2>&1 | head -3)
  [ -z "$derr" ] || errs+=("$codec : decoding errors : $derr")
  # content is not uniform (animated pattern): luminance spread of frame 150
  ffmpeg -v error -i "$f" -vf "select=eq(n\,150),signalstats,metadata=print:file=$out/t20.$codec.stats" -f null - 2>/dev/null
  ymin=$(sed -n 's/.*signalstats.YMIN=//p' "$out/t20.$codec.stats" | head -1)
  ymax=$(sed -n 's/.*signalstats.YMAX=//p' "$out/t20.$codec.stats" | head -1)
  info "[$codec] frame 150: Y min=$ymin max=$ymax"
  [ -n "$ymax" ] && [ $(( ${ymax%.*} - ${ymin%.*} )) -ge 64 ] || errs+=("$codec : frame 150 without contrast (min=$ymin max=$ymax)")
  rate=$(echo "$log" | sed -n 's/.*mean_bitrate=\([0-9.]*\).*/\1/p')
  real=$(ffprobe -v error -show_entries format=size -of csv=p=0 "$f")
  mbps=$(awk -v s="$real" 'BEGIN{printf "%.2f", s*8/5/1e6}')   # 300 frames at 60 fps = 5 s
  info "[$codec] bitrate measured by ffprobe = $mbps Mbit/s (bench: $rate)"
  awk -v m="$mbps" 'BEGIN{exit !(m>=18 && m<=22)}' || errs+=("$codec : bitrate $mbps Mbit/s outside 20±10 %")
  p99=$(echo "$log" | sed -n 's/.*p99=\([0-9.]*\).*/\1/p')
  awk -v p="${p99:-99}" 'BEGIN{exit !(p<16.6)}' || errs+=("$codec : latence p99=${p99:-?} ms")
done
if [ ${#errs[@]} -gt 0 ]; then printf '  # - %s\n' "${errs[@]}"; fail_or_skip N1 "${#errs[@]} discrepancy(ies) : ${errs[0]}"; fi
pass "H.264 and HEVC: decodable, IDR on demand, no B, CBR, p99 < 16.6 ms"
