#!/usr/bin/env bash
# T2 — Capture under Wine via IDXGIOutput1::DuplicateOutput (API assumed for
# Parsec, to align with docs/api-map.md), native test pattern displayed:
#   - 3 DXGI outputs = translated KWin layout, same primary;
#   - DuplicateOutput succeeds on each;
#   - non-black frame, matching the test pattern of the expected screen (physical order);
#   - >= 55 updates/s over 10 s on ONE output alone (animated pattern) (D-011); 3 streams: information;
#   - cursor shape obtained (GetFramePointerShape) on at least one output.
. "$(dirname "$0")/lib/common.sh"
need_cmd kscreen-doctor python3
exe=$(pe_bin dda_capture) || exit $?
need_prefix
need_intrusive "full-screen test pattern ~30 s"
no_client_connected
SECS=${PLH_CAPTURE_SECS:-10}
out="$TMPD/dda"; rm -rf "$out"; mkdir -p "$out"
# The harness starts and stops a private plh-bridge itself (KWin backend); the dxgi proxy finds it through PLH_BRIDGE_DIR.
trap stop_bridge EXIT
start_bridge "$TMPD/dda-rt" --capture-backend kwin
python3 "$TESTS_DIR/tools/pattern.py" --seconds $((2 * SECS + 14)) >"$out/pattern.log" 2>&1 &
mp=$!; sleep 3; "$TESTS_DIR/tools/kwin_raise.sh" plh-pattern
PE_TIMEOUT=$((SECS + 60)) run_pe "$exe" "$SECS" 'Z:\tmp\plh-critique\dda' >"$out/dda.log" 2>"$out/dda.err"; rc=$?
wait $mp
sed 's/^/  # /' "$out/dda.log"
[ $rc = 0 ] || fail_or_skip C4 "dda_capture.exe rc=$rc : $(tail -2 "$out/dda.err" | tr '\n' ' ')"
grep -q '^module dxgi.dll=' "$out/dda.log" && info "dxgi loaded: $(sed -n 's/^module dxgi.dll=//p' "$out/dda.log")"
map=$(python3 "$TESTS_DIR/tools/layout.py" <"$out/dda.log") || { echo "$map" | sed 's/^/  # /'; fail_or_skip C4 "DXGI outputs ≠ KWin layout"; }
echo "$map" | sed 's/^/  # /'
errs=()
nout=$(sed -n 's/^outputs=//p' "$out/dda.log")
[ "${nout:-0}" -ge 3 ] || errs+=("$nout DXGI outputs instead of 3")
while read -r idx _wname conn rank; do
  hr=$(sed -n "s/^dup idx=$idx hr=//p" "$out/dda.log")
  [ "$hr" = 0x00000000 ] || { errs+=("$conn : DuplicateOutput hr=$hr"); continue; }
  f="$out/output$idx.pam"
  if [ ! -s "$f" ]; then errs+=("$conn : no frame copied"); continue; fi
  r=$(python3 "$TESTS_DIR/tools/check_pattern.py" "$f" --expect-rank "$rank"); [ $? = 0 ] || errs+=("$conn : $r")
  info "$conn (output $idx) : $r"
  fps=$(sed -n "s/^frames idx=$idx .*fps=\([0-9.]*\).*/\1/p" "$out/dda.log")
  info "$conn : ${fps:-0} fps with 3 simultaneous streams (information, D-011)"
done < <(echo "$map" | grep '^map ' | cut -d' ' -f2-)
# D-011: 55 fps criterion on ONE duplicated output alone (2nd pass).
mkdir -p "$out/one"
PE_TIMEOUT=$((SECS + 60)) run_pe "$exe" "$SECS" 'Z:\tmp\plh-critique\dda\one' 0 >"$out/one.log" 2>"$out/one.err"
fps1=$(sed -n 's/^frames idx=0 .*fps=\([0-9.]*\).*/\1/p' "$out/one.log")
info "1 stream (output 0) : ${fps1:-0} fps"
awk -v f="${fps1:-0}" 'BEGIN{exit !(f>=55)}' || errs+=("1 stream: ${fps1:-0} fps < 55")
grep -qE '^pointer idx=[0-9]+ seen=1 .*shape_bytes=[1-9]' "$out/dda.log" || errs+=("no cursor shape received")
if [ ${#errs[@]} -gt 0 ]; then
  printf '  # - %s\n' "${errs[@]}"
  fail_or_skip C4 "${#errs[@]} discrepancy(ies) : ${errs[0]}"
fi
pass "DDA capture conforming on $nout outputs"
