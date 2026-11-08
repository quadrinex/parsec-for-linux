#!/usr/bin/env bash
# F2 end-to-end (owner: capture): all real outputs reported off (PLH_FAKE_SCREENS_OFF=1 on the bridge, PLH_VIRTUAL_DISPLAY_AUTO=1)
# -> DuplicateOutput1 on output 0 (the default screen) streams the VIRTUAL output and the proxy pushes the input redirect
# (default rect -> virtual rect); after Release + 3 s grace the output is gone. Also checks that WITHOUT AUTO the real output is used.
R=$(cd "$(dirname "$0")/../../../.." && pwd); T=/tmp/plh-capture; mkdir -p $T; cd "$R" || exit 2
x86_64-w64-mingw32-gcc -std=gnu11 -Wall -Wextra -o $T/vd_probe.exe src/capture/dxgi/test/vd_probe.c -ldxgi -ld3d11 -ldxguid -luuid -luser32 || exit 2
run() {
  bin/parsec-linux-host status | grep -q 'client *none' || { echo "SKIP a client is connected"; return 3; }
  [ "$(xrandr --listmonitors 2>/dev/null | grep -c Virtual-plh)" = 0 ] || { echo "SKIP a virtual output already exists"; return 3; }
  bin/parsec-linux-host stop >/dev/null 2>&1; sleep 2
  bash src/capture/dxgi/install-proxy.sh >/dev/null
  . runtime/env.sh
  export PLH_CAPTURE_BACKEND=kwin PLH_INPUT_BACKEND=none
  RD=/tmp/plh-capture/rt-vd-f2; rm -rf $RD; mkdir -m 700 -p $RD; export PLH_BRIDGE_DIR=$RD   # private bridge: never the host's runtime dir
  local ok=1
  for mode in f2 overlap noauto; do
    if [ $mode != noauto ]; then export PLH_VIRTUAL_DISPLAY=0 PLH_VIRTUAL_DISPLAY_AUTO=1 PLH_FAKE_SCREENS_OFF=1
    else export PLH_VIRTUAL_DISPLAY=0 PLH_VIRTUAL_DISPLAY_AUTO=0 PLH_FAKE_SCREENS_OFF=1; fi
    build/bridge/plh-bridge --runtime-dir $RD -v > $T/vd-f2.$mode.bridge.log 2>&1 & BP=$!
    for _ in $(seq 50); do [ -S $RD/bridge.sock ] && [ -s $RD/port ] && break; sleep 0.1; done
    timeout 90 wine $T/vd_probe.exe 2 f2 $([ $mode = overlap ] && echo overlap) 2>&1 | tr -d '\r' > $T/vd-f2.$mode.out
    left=$(xrandr --listmonitors 2>/dev/null | grep -c Virtual-plh)
    kill $BP; wait $BP 2>/dev/null; rm -f $RD/*
    if [ $mode = f2 ]; then
      grep -q 'F2: real screens off' $T/vd-f2.f2.out || { echo "FAIL f2: no F2 decision"; ok=0; }
      grep -q 'input redirect Windows 0,0,1920,1080 -> KWin' $T/vd-f2.f2.out || { echo "FAIL f2: no redirect for the default output"; ok=0; }
      grep -q 'sent=1' $T/vd-f2.f2.out || { echo "FAIL f2: redirect not sent"; ok=0; }
      grep -q 'output Virtual-plh-virtual' $T/vd-f2.f2.bridge.log || { echo "FAIL f2: stream not on the virtual output"; ok=0; }
      grep -Eq '^FRAMES [1-9]' $T/vd-f2.f2.out || { echo "FAIL f2: no frames"; ok=0; }
      grep -q 'released after the grace period' $T/vd-f2.f2.out || { echo "FAIL f2: no release after grace"; ok=0; }
      [ "$left" = 0 ] || { echo "FAIL f2: virtual output left behind"; ok=0; }
    elif [ $mode = overlap ]; then   # B created before A is released: A's Release must not clear the redirect
      awk '/OVERLAP_A_RELEASED/{a=1} /OVERLAP_B_ALIVE/{b=1} /input redirect kept/{if(a&&!b)k=1} /input redirect cleared/{if(!b)early=1; else late++} END{exit !(k&&!early&&late==1)}' $T/vd-f2.overlap.out \
        || { echo "FAIL overlap: redirect not kept while B lives, or not cleared exactly once after B"; ok=0; }
      grep -q 'DUP2 hr=0x00000000' $T/vd-f2.overlap.out || { echo "FAIL overlap: second duplication failed"; ok=0; }
      [ "$left" = 0 ] || { echo "FAIL overlap: virtual output left behind"; ok=0; }
    else
      grep -q 'F2:' $T/vd-f2.noauto.out && { echo "FAIL noauto: F2 triggered with AUTO=0"; ok=0; }
      grep -q 'Virtual-plh' $T/vd-f2.noauto.bridge.log && { echo "FAIL noauto: virtual output created with AUTO=0"; ok=0; }
      grep -q 'output \(DP\|HDMI\)' $T/vd-f2.noauto.bridge.log || { echo "FAIL noauto: the real output was not used"; ok=0; }
    fi
  done
  unset PLH_BRIDGE_DIR; rm -rf $RD
  bin/parsec-linux-host start >/dev/null 2>&1
  [ $ok = 1 ] && echo "PASS F2 screens off -> virtual output + redirect (and AUTO=0 leaves the real output)" || { echo "FAIL (see $T/vd-f2.*)"; return 1; }
}
flock -w 400 -o /tmp/plh-vout.lock flock -w 300 -o /tmp/plh-host.lock bash -c 'cd "'"$R"'"; export T='"$T"'; '"$(declare -f run)"'; run'
