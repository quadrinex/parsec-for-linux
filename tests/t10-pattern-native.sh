#!/usr/bin/env bash
# Self-check of the test pattern (without our stack): the native pattern is shown on
# each KWin screen and a reference capture (spectacle, allowed by KWin)
# finds it at the position of each connector. Proves that pattern + checker
# are reliable before using them to judge the capture under Wine (t11).
. "$(dirname "$0")/lib/common.sh"
need_intrusive "full-screen test pattern ~8 s"
need_cmd kscreen-doctor spectacle magick python3
python3 -c 'import PySide6' 2>/dev/null || skip "PySide6 absent"
out="$TMPD/pattern-ref"; rm -rf "$out"; mkdir -p "$out"
python3 "$TESTS_DIR/tools/pattern.py" --seconds 8 >"$out/pattern.log" 2>&1 &
mp=$!
sleep 3; "$TESTS_DIR/tools/kwin_raise.sh" plh-pattern; sleep 1
spectacle -b -n -f -o "$out/full.png" >/dev/null 2>&1; src=$?
wait $mp
cat "$out/pattern.log" | sed 's/^/  # /'
[ $src = 0 ] && [ -s "$out/full.png" ] || fail "spectacle produced no capture"
magick "$out/full.png" -alpha off "$out/full.ppm" || fail "PNG→PPM conversion"
info "reference capture: $(magick identify -format '%wx%h' "$out/full.png")"
bad=0; rank=0
# KWin screens sorted left to right: expected rank = x order.
while read -r name x y w h; do
  r=$(python3 "$TESTS_DIR/tools/check_pattern.py" "$out/full.ppm" --crop "$x,$y,$w,$h" --expect-rank $rank); rc=$?
  info "$name @ $x,$y : $r"
  [ $rc = 0 ] || bad=1
  rank=$((rank+1))
done < <(kscreen-doctor -j | python3 -c '
import json,sys
o=[o for o in json.load(sys.stdin)["outputs"] if o["enabled"]]
for o in sorted(o,key=lambda o:(o["pos"]["x"],o["pos"]["y"])):
    print(o["name"],o["pos"]["x"],o["pos"]["y"],o["size"]["width"],o["size"]["height"])')
[ $bad = 0 ] || fail "test pattern not found on at least one screen"
pass "test pattern seen by spectacle on the $rank screens at the KWin positions"
