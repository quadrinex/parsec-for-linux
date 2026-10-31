#!/bin/bash
# gamepad-e2e.sh — gamepad chain without Parsec or a client:
#   padprobe.exe (ViGEmClient-like call sequence) -> plh-vigem.sys (Wine winedevice) -> plh-pad-relay.exe
#   -> private plh-bridge (input service) -> uinput Xbox 360 pad -> read back from evdev.
# Verifies plug, buttons, D-pad, sticks, triggers and the device removal on unplug.
# Uses a throwaway Wine prefix ($PLH_PAD_PREFIX, default /var/tmp/plh-gamepad/pfx). TAP output.
# Injects only into its own virtual pad, but takes the input lock anyway (one injector at a time).
set -u
exec 8>/tmp/plh-input.lock
flock -w 120 8 || { echo "Bail out! lock /tmp/plh-input.lock unavailable"; exit 1; }
R=$(cd "$(dirname "$0")/../../.." && pwd); T=/tmp/plh-entrees-pad; mkdir -p "$T"
B=$R/build
. "$R/runtime/env.sh"
export WINEPREFIX=${PLH_PAD_PREFIX:-/var/tmp/plh-gamepad/pfx} WINEDEBUG=-all PLH_BRIDGE_DIR=$T/br-$$
n=0; fails=0
ok()  { n=$((n+1)); echo "ok $n - $*"; }
nok() { n=$((n+1)); fails=$((fails+1)); echo "not ok $n - $*"; }
skip() { echo "1..0 # SKIP $*"; exit 0; }
[ -w /dev/uinput ] || skip "/dev/uinput not accessible"
for f in input/pe/plh-vigem.sys input/pe/plh-pad-relay.exe input/pe/padprobe.exe input/pad-evdev-dump bridge/plh-bridge; do
    [ -e "$B/$f" ] || { echo "Bail out! missing build/$f (run make)"; exit 1; }
done
[ -f "$WINEPREFIX/system.reg" ] || { mkdir -p "$(dirname "$WINEPREFIX")"; timeout 300 "$WINE" wineboot -i >/dev/null 2>&1; }

mkdir -p "$PLH_BRIDGE_DIR"; chmod 700 "$PLH_BRIDGE_DIR"
PLH_INPUT_BACKEND=uinput "$B/bridge/plh-bridge" --runtime-dir "$PLH_BRIDGE_DIR" --capture-backend test > "$T/bridge.log" 2>&1 & bp=$!
for _ in $(seq 50); do [ -e "$PLH_BRIDGE_DIR/port" ] && break; sleep 0.1; done; sleep 1
timeout 90 "$WINE" "$B/input/pe/plh-pad-relay.exe" --install "$B/input/pe/plh-vigem.sys" --seconds 40 -v > "$T/relay.log" 2>&1 & rp=$!
for _ in $(seq 150); do grep -q READY "$T/relay.log" 2>/dev/null && break; sleep 0.2; done
grep -q READY "$T/relay.log" && ok "driver installed, bus interface published (relay READY)" || { nok "relay did not become READY"; cat "$T/relay.log"; }

"$B/input/pad-evdev-dump" 20 30 > "$T/evdev.log" 2>&1 & dp=$!
timeout 60 "$WINE" "$B/input/pe/padprobe.exe" 400 > "$T/probe.log" 2>&1
wait $dp
sed 's/^/# probe: /' "$T/probe.log"
grep -q "^open: ok" "$T/probe.log" && grep -q "^check version: ok" "$T/probe.log" && grep -q "^plug in: ok" "$T/probe.log" && ok "ViGEm bus: open, check version, plug in accepted" || nok "bus protocol steps"
grep -q "^notification: pending" "$T/probe.log" && ok "notification request stays pending" || nok "notification request not pending"
if grep -q NODEVICE "$T/evdev.log"; then nok "no virtual pad appeared"; sed 's/^/# evdev: /' "$T/evdev.log"
elif grep -q OPENFAIL "$T/evdev.log"; then echo "ok $((n+1)) - # SKIP cannot read the evdev node"; n=$((n+1))
else
    sed 's/^/# evdev: /' "$T/evdev.log"
    has() { grep -qx "EV $1" "$T/evdev.log"; }
    # EV_KEY=1 EV_ABS=3 ; codes: A=304 B=305 X=307 Y=308 TL=310 TR=311 SELECT=314 START=315 MODE=316 THUMBL=317 THUMBR=318
    has "1 304 1" && has "3 17 -1" && ok "report 0: A pressed, D-pad up (HAT0Y=-1)" || nok "report 0 buttons"
    has "3 2 255" && ok "report 0: left trigger 255 (ABS_Z)" || nok "report 0 trigger"
    has "3 0 32767" && has "3 1 32767" && ok "report 0: LX max, LY min (ABS_Y inverted like xpad)" || nok "report 0 sticks"
    has "1 305 1" && has "3 17 0" && has "1 310 1" && has "3 16 1" && ok "report 1: B, LB, D-pad right" || nok "report 1 buttons"
    has "3 5 128" && has "3 0 -32768" && has "3 1 -32768" && ok "report 1: RT 128, LX min, LY max" || nok "report 1 axes"
    has "3 3 1000" && has "3 4 999" && ok "report 1: right stick" || nok "report 1 right stick"
    has "3 3 32767" && has "3 4 32767" && ok "report 2: right stick max" || nok "report 2 right stick"
    has "1 307 1" && has "1 308 1" && has "1 311 1" && has "1 315 1" && has "1 314 1" && has "1 317 1" && has "1 318 1" && has "1 316 1" && ok "report 2: X Y RB START SELECT L3 R3 GUIDE" || nok "report 2 buttons"
    has "1 304 0" && has "1 316 0" && ok "report 3: all released" || nok "report 3 release"
    grep -qx REMOVED "$T/evdev.log" && ok "virtual pad removed after unplug" || nok "pad not removed after unplug"
fi
# client dies without unplugging: closing the handle must remove the pad
"$B/input/pad-evdev-dump" 20 20 > "$T/evdev2.log" 2>&1 & dp=$!
timeout 60 "$WINE" "$B/input/pe/padprobe.exe" 100 leave > "$T/probe2.log" 2>&1
wait $dp
grep -qx REMOVED "$T/evdev2.log" && ok "pad removed when the ViGEm client exits without unplugging" || { nok "pad left behind after client exit"; sed 's/^/# evdev2: /' "$T/evdev2.log"; }
# relay killed (host stop): the bridge must drop the pad
"$B/input/pad-evdev-dump" 20 25 > "$T/evdev3.log" 2>&1 & dp=$!
timeout 60 "$WINE" "$B/input/pe/padprobe.exe" 100 leave > "$T/probe3.log" 2>&1 & pp=$!
for _ in $(seq 100); do grep -q "^FOUND" "$T/evdev3.log" && break; sleep 0.1; done; sleep 0.5
kill $rp 2>/dev/null; wait $rp 2>/dev/null; "$WINESERVER" -k 2>/dev/null
wait $dp; wait $pp 2>/dev/null
grep -qx REMOVED "$T/evdev3.log" && ok "pad removed when the relay is stopped" || { nok "pad left behind after relay stop"; sed 's/^/# evdev3: /' "$T/evdev3.log"; }
kill $rp 2>/dev/null; wait $rp 2>/dev/null; "$WINESERVER" -k 2>/dev/null
kill $bp 2>/dev/null; wait $bp 2>/dev/null
rm -rf "$PLH_BRIDGE_DIR"
echo "1..$n"; exit $((fails != 0))
