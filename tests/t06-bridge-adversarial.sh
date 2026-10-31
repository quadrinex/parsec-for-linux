#!/usr/bin/env bash
# Adversarial review of the plh-bridge daemon (capture, C1) on a PRIVATE instance
# (temporary runtime dir, test-pattern backend) — never the user's.
# The TCP port is reachable by ANY local user: we play a hostile local client.
#   1. random bytes / size 0xFFFFFFFF / message before HELLO / wrong token → connection closed;
#   2. half a message then close → daemon alive;
#   3. flood: 1100 idle unauthenticated TCP connections, daemon limited to
#      1024 descriptors (default of a systemd --user service) → during the attack, a
#      legitimate client must still be served and the daemon must not spin (CPU);
#   4. after the attack: no descriptor leak, legitimate client served.
. "$(dirname "$0")/lib/common.sh"
B="$REPO/build/bridge"
[ -x "$B/plh-bridge" ] && [ -x "$B/plh-captest" ] || fail_or_skip C1 "plh-bridge not built ($B)"
rd="$TMPD/bridge-adv"; rm -rf "$rd"; mkdir -m 700 "$rd"
( ulimit -n 1024; exec "$B/plh-bridge" --runtime-dir "$rd" --capture-backend test ) >"$TMPD/bridge-adv.log" 2>&1 &
bp=$!
trap 'kill $bp 2>/dev/null; wait $bp 2>/dev/null' EXIT
for _ in $(seq 50); do [ -S "$rd/bridge.sock" ] && [ -s "$rd/port" ] && break; sleep 0.1; done
[ -s "$rd/port" ] || fail "private daemon not started : $(tail -3 "$TMPD/bridge-adv.log")"
A="python3 $TESTS_DIR/tools/bridge_attack.py $rd"
errs=()
legit() { PLH_BRIDGE_DIR="$rd" PLH_BRIDGE_FORCE_TCP=1 timeout 20 "$B/plh-captest" -b test -n 30 -q 2>&1 | grep -c '^RESULT.*counter_errors=0'; }
for c in garbage hugesize prehello badtoken halfmsg; do
  r=$($A $c 2>&1) || errs+=("$c : $r"); info "$r"
  kill -0 $bp 2>/dev/null || { errs+=("daemon dead after \"$c\""); fail "${errs[*]}"; }
done
fd0=$(ls /proc/$bp/fd | wc -l); info "daemon descriptors at rest : $fd0"
$A flood 1100 12 >"$TMPD/flood.log" 2>&1 &
fp=$!; sleep 4
info "$(cat "$TMPD/flood.log")"
nflood=$(sed -n 's/^flood: \([0-9]*\) idle unauthenticated.*/\1/p' "$TMPD/flood.log")
grep -q '^flood: failed' "$TMPD/flood.log" && errs+=("the daemon stops accepting TCP connections after ~$(sed -n 's/.*failed at connection \([0-9]*\).*/\1/p' "$TMPD/flood.log") unauthenticated local connections (no limit or authentication timeout)")
fdf=$(ls /proc/$bp/fd | wc -l)
t0=$(awk '{print $14+$15}' /proc/$bp/stat); sleep 2; t1=$(awk '{print $14+$15}' /proc/$bp/stat)
cpu=$(( (t1 - t0) * 100 / $(getconf CLK_TCK) / 2 ))
info "during the attack: descriptors=$fdf, daemon CPU ≈ ${cpu} %"
[ $cpu -lt 50 ] || errs+=("daemon at ${cpu} % CPU during the flood (accept loop on EMFILE?)")
n=$(legit); info "legitimate client (TCP+token) during the attack: $n output(s) served"
[ "${n:-0}" -ge 1 ] || errs+=("legitimate client refused during the flood by a local user (denial of service)")
wait $fp
sleep 2
fd1=$(ls /proc/$bp/fd | wc -l); info "descriptors after the attack : $fd1"
[ $fd1 -le $((fd0 + 4)) ] || errs+=("leak: $fd0 → $fd1 descriptors after closing the connections")
n=$(legit); [ "${n:-0}" -ge 1 ] || errs+=("legitimate client refused after the attack")
kill -0 $bp 2>/dev/null || errs+=("daemon dead")
if [ ${#errs[@]} -gt 0 ]; then printf '  # - %s\n' "${errs[@]}"; fail "${#errs[@]} weakness(es) : ${errs[0]}"; fi
pass "daemon robust: invalid frames rejected, local flood without denial of service or leak"
