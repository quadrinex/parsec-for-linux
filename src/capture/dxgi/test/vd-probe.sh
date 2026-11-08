#!/usr/bin/env bash
# F1 end-to-end (owner: capture): placeholder "Virtual display" in DXGI EnumOutputs -> DuplicateOutput1 -> KWin virtual
# output + frames -> Release -> 3 s grace -> output gone and placeholder offered again. Uses a PRIVATE-ish bridge on the default
# runtime dir (the host is stopped meanwhile and restarted at the end). Takes /tmp/plh-vout.lock and /tmp/plh-host.lock.
# Needs: no client connected. Output: PASS/FAIL line; details in /tmp/plh-capture/vd-probe.out
R=$(cd "$(dirname "$0")/../../../.." && pwd); T=/tmp/plh-capture; mkdir -p $T; cd "$R" || exit 2
x86_64-w64-mingw32-gcc -std=gnu11 -Wall -Wextra -o $T/vd_probe.exe src/capture/dxgi/test/vd_probe.c -ldxgi -ld3d11 -ldxguid -luuid -luser32 || exit 2
run() {
  bin/parsec-linux-host status | grep -q 'client *none' || { echo "SKIP a client is connected"; return 3; }
  [ "$(xrandr --listmonitors 2>/dev/null | grep -c Virtual-plh)" = 0 ] || { echo "SKIP a virtual output already exists"; return 3; }
  bin/parsec-linux-host stop >/dev/null 2>&1; sleep 2
  bash src/capture/dxgi/install-proxy.sh >/dev/null
  . runtime/env.sh
  # test env scoped to the private bridge and probe commands only (never exported into this shell, never into the host)
  RD=/tmp/plh-capture/rt-vd-probe; rm -rf $RD; mkdir -m 700 -p $RD
  local TENV="PLH_VIRTUAL_DISPLAY=1 PLH_CAPTURE_BACKEND=kwin PLH_INPUT_BACKEND=none PLH_BRIDGE_DIR=$RD"
  env $TENV build/bridge/plh-bridge --runtime-dir $RD -v > $T/vd-probe.bridge.log 2>&1 & BP=$!
  for _ in $(seq 50); do [ -S $RD/bridge.sock ] && [ -s $RD/port ] && break; sleep 0.1; done
  env $TENV timeout 90 wine $T/vd_probe.exe 4 2>&1 | tr -d '\r' > $T/vd-probe.out
  left=$(xrandr --listmonitors 2>/dev/null | grep -c Virtual-plh)
  # R5: second release must also be followed by its own grace destroy (release, re-dup in grace, hold 4.5 s, release, wait)
  rm -f $T/vd-probe.regrace.out; : > $T/vd-probe.regrace.samples
  env $TENV timeout 90 wine $T/vd_probe.exe 2 f2 regrace 2>&1 | tr -d '\r' | while IFS= read -r l; do
    echo "$l" >> $T/vd-probe.regrace.out
    case $l in "SAMPLE "*) echo "${l#SAMPLE }=$(xrandr --listmonitors 2>/dev/null | grep -c Virtual-plh)" >> $T/vd-probe.regrace.samples;; esac
  done
  kill $BP; wait $BP 2>/dev/null; rm -rf $RD
  env -u PLH_BRIDGE_DIR -u PLH_VIRTUAL_DISPLAY -u PLH_VIRTUAL_DISPLAY_AUTO -u PLH_FAKE_SCREENS_OFF -u PLH_CAPTURE_BACKEND -u PLH_INPUT_BACKEND \
    bin/parsec-linux-host start >/dev/null 2>&1
  local ok=1
  grep -q '^held_after_first_deadline=1' $T/vd-probe.regrace.samples || { echo "FAIL regrace: output not alive while held past the first deadline"; ok=0; }
  grep -q '^after_second_grace=0' $T/vd-probe.regrace.samples || { echo "FAIL regrace (R5): output still present 5.5 s after the second release"; ok=0; }
  [ "$(grep -c 'released after the grace period' $T/vd-probe.regrace.out)" = 1 ] || { echo "FAIL regrace (R5): expected exactly one grace release in the proxy log"; ok=0; }
  # env leak check: the host bridge must carry none of the test variables
  . runtime/bridge-pid.sh; sleep 1
  hp=$(plh_host_bridge_pid) && { tr '\0' '\n' < /proc/$hp/environ | grep -E '^PLH_(BRIDGE_DIR|VIRTUAL_DISPLAY|VIRTUAL_DISPLAY_AUTO|FAKE_SCREENS_OFF)=' && { echo "FAIL host bridge inherited test env"; ok=0; }; } || { echo "FAIL host bridge not running after restart"; ok=0; }
  grep -q '^COUNT before dxgi=4 wine_monitors=3' $T/vd-probe.out || { echo "FAIL placeholder not listed as 4th output before"; ok=0; }
  grep -q '^OUT before 3 .*DISPLAY4 ' $T/vd-probe.out || ok=0
  grep -q '^DUP hr=0x00000000' $T/vd-probe.out || { echo "FAIL DuplicateOutput1 on the placeholder"; ok=0; }
  grep -Eq '^FRAMES [1-9]' $T/vd-probe.out || { echo "FAIL no frames"; ok=0; }
  grep -q 'released after the grace period' $T/vd-probe.out || { echo "FAIL no release after grace"; ok=0; }
  grep -q '^COUNT after5s dxgi=4 wine_monitors=3' $T/vd-probe.out || { echo "FAIL placeholder not back after release"; ok=0; }
  [ "$left" = 0 ] || { echo "FAIL virtual output left behind"; ok=0; }
  [ $ok = 1 ] && echo "PASS F1 placeholder -> virtual output -> frames -> release -> gone" || { echo "FAIL (see $T/vd-probe.out)"; return 1; }
}
flock -w 400 -o /tmp/plh-vout.lock flock -w 300 -o /tmp/plh-host.lock bash -c 'cd "'"$R"'"; export T='"$T"'; '"$(declare -f run)"'; run'
