#!/usr/bin/env bash
# Runs tests/t*.sh (or those given as arguments) and produces TAP output.
# Exit code: 0 if no FAIL, 1 otherwise (with STRICT=1: also 1 if any SKIP remains). SKIPs do not fail the run,
# but they are counted and listed: a SKIP is never a PASS.
set -u
cd "$(dirname "$0")"
mapfile -t TESTS < <(if [ $# -gt 0 ]; then printf '%s\n' "$@"; else ls t[0-9][0-9]-*.sh; fi)
LOGDIR=/tmp/plh-critique/logs; mkdir -p "$LOGDIR"
echo "TAP version 13"
echo "1..${#TESTS[@]}"
n=0; np=0; nf=0; ns=0; fails=(); skips=()
for t in "${TESTS[@]}"; do
  n=$((n+1)); name="${t%.sh}"; log="$LOGDIR/$name.log"
  start=$(date +%s%N)
  bash "$t" >"$log" 2>&1; rc=$?
  ms=$(( ($(date +%s%N) - start) / 1000000 )); dur="$((ms/1000)).$(( (ms%1000)/100 ))"
  last=$(grep -E '# (PASS|FAIL|SKIP):' "$log" | tail -1 | sed 's/^ *# //')
  case $rc in
    0)  np=$((np+1)); echo "ok $n - $name (${dur}s)";;
    77) ns=$((ns+1)); skips+=("$name"); echo "ok $n - $name # SKIP ${last#SKIP: }";;
    *)  nf=$((nf+1)); fails+=("$name"); echo "not ok $n - $name (rc=$rc, ${dur}s)"
        [ -n "$last" ] || last="FAIL: code $rc without message"
        ;;
  esac
  # Details (prefixed by #): always for FAIL, otherwise in verbose mode.
  if [ $rc -ne 0 ] && [ $rc -ne 77 ] || [ "${V:-0}" = 1 ]; then sed 's/^/    /' "$log"; fi
done
echo "# PASS=$np FAIL=$nf SKIP=$ns (logs: $LOGDIR)"
[ $nf -eq 0 ] || echo "# failures: ${fails[*]}"
[ $ns -eq 0 ] || echo "# skipped: ${skips[*]}"
# STRICT=1 (make test-full): the slightest SKIP fails the run.
if [ "${STRICT:-0}" = 1 ] && [ $ns -gt 0 ]; then echo "# STRICT: $ns test(s) not run → failure"; exit 1; fi
[ $nf -eq 0 ]
