#!/usr/bin/env bash
# T5 — WASAPI loopback (default output) under Wine not silent while
# a 440 Hz sine is played natively (volume 5 %, 2 s, default output),
# and almost silent without it (the baseline level is reported, not required:
# the user may be listening to something).
. "$(dirname "$0")/lib/common.sh"
need_cmd pw-play python3
exe=$(pe_bin wasapi_loopback) || exit $?
need_prefix
need_intrusive "brief 440 Hz sine at 5 % on the default output"
out="$TMPD/audio"; mkdir -p "$out"
python3 - "$out/sine.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", v, v) for v in (int(12000 * math.sin(2 * math.pi * 440 * i / 48000)) for i in range(48000 * 2))))
PY
base=$(run_pe "$exe" 1.5 2>/dev/null); info "without sound: $(echo "$base" | grep captured)"
( sleep 1.0; pw-play --volume 0.05 "$out/sine.wav" ) &
pp=$!
res=$(PE_TIMEOUT=30 run_pe "$exe" 4 2>"$out/err.log"); rc=$?
wait $pp
echo "$res" | sed 's/^/  # /'
[ $rc = 0 ] || fail_or_skip E4 "wasapi_loopback rc=$rc ($(tail -2 "$out/err.log" | tr '\n' ' '))"
peak=$(echo "$res" | sed -n 's/.*peak=\([0-9.]*\).*/\1/p')
# sine at 12000/32768 * 0.05 (stream volume) * sink volume ≈ 0.01; threshold 0.002
awk -v p="${peak:-0}" 'BEGIN{exit !(p>=0.002)}' || fail_or_skip E4 "loopback silent (peak ${peak:-?}) while a sine is playing"
pass "non-silent WASAPI loopback (peak $peak)"
