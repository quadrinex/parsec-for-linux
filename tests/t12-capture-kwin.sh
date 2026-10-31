#!/usr/bin/env bash
# C3 — capture of the REAL desktop by the bridge's direct KWin backend (plh-bridge
# --capture-backend kwin), animated native test pattern displayed, native client AND PE client:
#   - one output per KWin screen, exact KWin positions;
#   - >= 55 fps over 10 s on ONE output alone (D-011; 3 streams: information) (the pattern changes every frame);
#   - last frame of each output matching the test pattern of that screen (check_pattern);
#   - no lost frame / inconsistent counter reported by plh-captest.
# PRIVATE daemon (temporary runtime dir), never the user's.
. "$(dirname "$0")/lib/common.sh"
need_cmd kscreen-doctor magick python3
B="$REPO/build/bridge"
[ -x "$B/plh-bridge" ] && [ -x "$B/plh-captest" ] || fail_or_skip C3 "plh-bridge/plh-captest not built"
need_intrusive "full-screen test pattern ~25 s"
SECS=${PLH_CAPTURE_SECS:-10}
out="$TMPD/kwin"; rm -rf "$out"; mkdir -p "$out"; rd="$out/rt"; mkdir -m 700 "$rd"
"$B/plh-bridge" --runtime-dir "$rd" --capture-backend kwin >"$out/bridge.log" 2>&1 &
bp=$!
python3 "$TESTS_DIR/tools/pattern.py" --seconds $((3 * SECS + 20)) >"$out/pattern.log" 2>&1 &
mp=$!
trap 'kill $bp $mp 2>/dev/null; wait 2>/dev/null' EXIT
for _ in $(seq 50); do [ -s "$rd/port" ] && break; sleep 0.1; done
[ -s "$rd/port" ] || fail_or_skip C3 "private daemon not started : $(tail -2 "$out/bridge.log")"
sleep 3; "$TESTS_DIR/tools/kwin_raise.sh" plh-pattern
errs=()
check_client() { # name, command...
  local name=$1; shift
  local dir="$out/$name"; mkdir -p "$dir/img"
  PLH_BRIDGE_DIR="$rd" timeout $((SECS + 40)) "$@" >"$dir/log" 2>&1; local rc=$?
  tr -d '\r' <"$dir/log" | grep -E '^(RESULT|output|ERR|error)' | sed "s/^/  # [$name] /"
  [ $rc = 0 ] || { errs+=("$name : plh-captest rc=$rc ($(tail -1 "$dir/log" | tr -d '\r'))"); return; }
  while read -r name_out x y; do
    line=$(tr -d '\r' <"$dir/log" | grep "^RESULT output=$name_out ")
    [ -n "$line" ] || { errs+=("$name : no RESULT for $name_out"); continue; }
    echo "$line" | grep -q "pos=$x,$y " || errs+=("$name : $name_out position ≠ KWin $x,$y")
    fps=$(echo "$line" | sed -n 's/.* fps=\([0-9.]*\).*/\1/p')
    info "[$name] $name_out : ${fps:-0} fps with 3 simultaneous streams (information, D-011)"
    echo "$line" | grep -qE 'lost=0( |$)' || errs+=("$name : $name_out lost frames ($(echo "$line" | grep -o 'lost=[0-9]*'))")
  done < <(kscreen-doctor -j | python3 -c '
import json,sys
for o in sorted([o for o in json.load(sys.stdin)["outputs"] if o["enabled"]], key=lambda o:o["pos"]["x"]):
    print(o["name"],o["pos"]["x"],o["pos"]["y"])')
  rank=0
  for conn in $(kscreen-doctor -j | python3 -c '
import json,sys
print(" ".join(o["name"] for o in sorted([o for o in json.load(sys.stdin)["outputs"] if o["enabled"]], key=lambda o:o["pos"]["x"])))'); do
    f="$dir/img/$conn.png"
    if [ -s "$f" ]; then
      magick "$f" -alpha off "$dir/$conn.ppm"
      r=$(python3 "$TESTS_DIR/tools/check_pattern.py" "$dir/$conn.ppm" --expect-rank $rank); [ $? = 0 ] || errs+=("$name : $conn $r")
      info "[$name] $conn : $r"
    else errs+=("$name : no image $f"); fi
    rank=$((rank+1))
  done
}
check_client native "$B/plh-captest" -b kwin -t "$SECS" -o "$out/native/img"
# D-011: 55 fps criterion judged on ONE output at a time (id 1 = DP-2, primary), animated pattern.
one=$(PLH_BRIDGE_DIR="$rd" timeout $((SECS + 40)) "$B/plh-captest" -b kwin -t "$SECS" -s 1 -o "$out/one" 2>&1 | tr -d '\r' | grep '^RESULT')
fps1=$(echo "$one" | sed -n 's/.* fps=\([0-9.]*\).*/\1/p' | head -1); info "1 stream (id 1) : ${fps1:-0} fps"
awk -v f="${fps1:-0}" 'BEGIN{exit !(f>=55)}' || errs+=("1 stream: ${fps1:-0} fps < 55")
if [ -x "$B/plh-captest.exe" ] && [ -f "$PREFIX_DIR/system.reg" ]; then
  wine_env
  check_client pe env PLH_BRIDGE_DIR="$rd" "$WINE" "$B/plh-captest.exe" -b kwin -t "$SECS" -o "Z:${out//\//\\}\\pe\\img"
else info "PE client not tested (exe or prefix missing)"; fi
if [ ${#errs[@]} -gt 0 ]; then printf '  # - %s\n' "${errs[@]}"; fail_or_skip C3 "${#errs[@]} discrepancy(ies) : ${errs[0]}"; fi
pass "KWin capture: outputs at the KWin positions, >= 55 fps, test pattern conforming (native + PE)"
