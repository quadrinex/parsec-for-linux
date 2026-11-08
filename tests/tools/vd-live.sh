#!/usr/bin/env bash
# Live virtual display cases for t42 (intrusive: stops and restarts the host, creates a real KWin output).
# usage: vd-live.sh grace   -> 5a regrace, 5b twice, 5c crash of the DXGI client (private bridge, host stopped meanwhile)
# Locks: /tmp/plh-vout.lock then /tmp/plh-host.lock (flock -o). Refuses to run with a client connected.
# Prints "ok"/"not ok" TAP-ish lines; exit 0 only if all pass, 77 if it cannot run, 1 otherwise.
R=$(cd "$(dirname "$0")/../.." && pwd); cd "$R" || exit 2
T=/tmp/plh-critique/vdlive; mkdir -p "$T"
n=0; fails=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fails=$((fails+1)); echo "not ok $n - $*"; }
vc() { xrandr --listmonitors 2>/dev/null | grep -c Virtual-plh; }
exec 7>/tmp/plh-vout.lock; flock -w 180 7 || { echo "Bail out! vout lock"; exit 1; }
exec 6>/tmp/plh-host.lock; flock -w 300 6 || { echo "Bail out! host lock"; exit 1; }
HOST=$R/bin/parsec-linux-host
"$HOST" status 2>/dev/null | grep -q '^client *none' || { echo "SKIP a Parsec client is connected (or status unreadable)"; exit 77; }
[ "$(vc)" = 0 ] || { echo "Bail out! stray Virtual-plh output before the test"; exit 1; }
EXE=$R/build/tests/pe/vdgrace.exe
[ -f "$EXE" ] || { echo "SKIP vdgrace.exe not built"; exit 77; }
BP=0; RD=
restore() {
  [ "$BP" -gt 0 ] && kill "$BP" 2>/dev/null; [ "$BP" -gt 0 ] && wait "$BP" 2>/dev/null
  ( . "$R/runtime/env.sh"; "$WINESERVER" -k ) 2>/dev/null
  [ -n "$RD" ] && rm -rf "$RD"
  # clean environment: the test's PLH_BRIDGE_DIR / PLH_VIRTUAL_DISPLAY must not leak into the host
  env -u PLH_BRIDGE_DIR -u PLH_VIRTUAL_DISPLAY -u PLH_VIRTUAL_DISPLAY_AUTO -u PLH_FAKE_SCREENS_OFF -u PLH_CAPTURE_BACKEND -u PLH_INPUT_BACKEND \
    "$HOST" start >/dev/null 2>&1 6>&- 7>&-
}
trap restore EXIT; trap "exit 143" TERM INT
"$HOST" stop >/dev/null 2>&1; sleep 2
bash src/capture/dxgi/install-proxy.sh >/dev/null || { echo "Bail out! install-proxy failed"; exit 1; }
. runtime/env.sh
export PLH_VIRTUAL_DISPLAY=1 PLH_CAPTURE_BACKEND=kwin PLH_INPUT_BACKEND=none
RD=$(mktemp -d "$T/rt.XXXXXX"); chmod 700 "$RD"; export PLH_BRIDGE_DIR=$RD
start_bridge() { build/bridge/plh-bridge --runtime-dir "$RD" -v >>"$T/bridge.log" 6>&- 7>&- 2>&1 & BP=$!
  for _ in $(seq 60); do [ -S "$RD/bridge.sock" ] && [ -s "$RD/port" ] && return 0; sleep 0.1; done; return 1; }
: >"$T/bridge.log"
start_bridge || { echo "Bail out! private bridge did not start"; exit 1; }

# run <mode> -> $T/<mode>.out, with samples in $T/<mode>.samples (tag=count)
run_mode() {
  : >"$T/$1.samples"
  timeout 90 wine "$EXE" "$1" 6>&- 7>&- 2>&1 </dev/null | sed -u 's/\r$//' | while IFS= read -r line; do
    echo "$line" >>"$T/$1.out"
    case $line in "SAMPLE "*) echo "${line#SAMPLE }=$(vc)" >>"$T/$1.samples";; esac
  done
}
sv() { sed -n "s/^$2=//p" "$T/$1.samples" | tail -1; }

# 5a. release, then a second duplication INSIDE the grace period: the output must stay, the stale first timer must not kill it
rm -f "$T/regrace.out"; run_mode regrace
grep -q '^DUP 1 hr=0x00000000' "$T/regrace.out" 2>/dev/null || nok "5a: first DuplicateOutput1 on the placeholder failed: $(grep -E 'DUP|FAIL|COUNT' "$T/regrace.out" | tr '\n' ' ')"
for kv in "a_held=1" "b_in_grace=1" "c_second_dup=1" "d_after_first_timer=1" "e_after_second_grace=0"; do
  tag=${kv%%=*}; want=${kv#*=}; got=$(sv regrace "$tag")
  [ "$got" = "$want" ] && ok "5a: $tag Virtual-plh outputs = $got" || nok "5a: $tag expected $want virtual output(s), got '${got:-none}'"
done
grep -q '^DUP 2 hr=0x00000000' "$T/regrace.out" && ok "5a: second duplication inside the grace period succeeded" || nok "5a: second duplication failed"
f2=$(sed -n 's/^FRAMES2 //p' "$T/regrace.out"); [ "${f2:-0}" -ge 1 ] && ok "5a: frames after re-duplication ($f2)" || nok "5a: no frame after re-duplication"
c=$(grep '^COUNT ingrace' "$T/regrace.out" | awk '{print $3}'); [ "$c" = 4 ] && ok "5a: placeholder offered exactly once during the grace period (4 outputs)" || nok "5a: during grace the DXGI list has '$c' outputs (expected 3 real + 1 placeholder)"
[ "$(vc)" = 0 ] && ok "5a: nothing left behind" || nok "5a: output left behind"

# 5b. listing twice with nothing duplicated: placeholder exactly once each time, no output created by listing alone
rm -f "$T/twice.out"; run_mode twice
grep -q '^TWICE 4 4' "$T/twice.out" && ok "5b: placeholder listed once in both enumerations (4, 4)" || nok "5b: $(grep -E 'TWICE|COUNT' "$T/twice.out" | tr '\n' ' ')"
[ "$(vc)" = 0 ] && ok "5b: enumerating alone creates no output" || nok "5b: enumeration created a virtual output"

# 5c. the DXGI client dies (SIGKILL, a Parsec crash) while holding the duplication: the bridge must drop the output
rm -f "$T/hold.out"
( timeout 90 wine "$EXE" hold 6>&- 7>&- 2>&1 </dev/null | sed -u 's/\r$//' >"$T/hold.out" ) &
HPID=$!
for _ in $(seq 100); do grep -q '^HOLDING' "$T/hold.out" 2>/dev/null && break; sleep 0.2; done
if grep -q '^HOLDING' "$T/hold.out"; then
  [ "$(vc)" = 1 ] && ok "5c: output exists while the client holds the duplication" || nok "5c: no output while holding"
  pkill -KILL -f 'vdgrac[e]\.exe'
  gone=0; for _ in $(seq 40); do [ "$(vc)" = 0 ] && { gone=1; break; }; sleep 0.25; done
  [ $gone = 1 ] && ok "5c: output removed after SIGKILL of the DXGI client" || nok "5c: output left behind 10 s after the client was killed"
else nok "5c: client never reached HOLDING: $(tail -3 "$T/hold.out" | tr '\n' ' ')"; fi
wait "$HPID" 2>/dev/null
echo "# 1..$n  fails=$fails"
[ $fails = 0 ]
