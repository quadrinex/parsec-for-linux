#!/usr/bin/env bash
# T31 — Virtual gamepad chain (ViGEmBus-compatible driver under Wine -> relay -> plh-bridge -> uinput Xbox 360 pad).
#  A. src/input/tests/gamepad-e2e.sh (takes the input lock itself): every check must be "ok", none skipped,
#     and the run must not leave a virtual pad behind.
#  B. adversarial driver client (pe/pad_malformed.c): malformed and hostile IOCTLs must be refused with an error,
#     must not crash or hang the driver/relay, must not leak a pad, and a normal cycle must still work afterwards.
#  C. leak after a killed relay: the relay is SIGKILLed while the client is still plugged; the uinput pad must
#     disappear by itself (the client keeps running, so the driver's own cleanup cannot be what removes it).
# Only our own virtual pad is created. Throwaway Wine prefix /var/tmp/plh-gamepad/pfx (never the project prefix).
. "$(dirname "$0")/lib/common.sh"
need_cmd flock pgrep
need_intrusive "creates virtual gamepads (uinput) for a few minutes"
[ -w /dev/uinput ] || skip "/dev/uinput not accessible"
no_client_connected
B="$REPO/build"
for f in input/pe/plh-vigem.sys input/pe/plh-pad-relay.exe input/pe/padprobe.exe input/pad-evdev-dump bridge/plh-bridge tests/pe/pad_malformed.exe; do
  [ -e "$B/$f" ] || fail "missing build/$f (make, then make -C tests build)"
done
out="$TMPD/pad"; rm -rf "$out"; mkdir -p "$out"
pads() { grep -c 'X-Box 360 pad (PLH' /proc/bus/input/devices; }
pad0=$(pads)
errs=()
PFX="${PLH_PAD_PREFIX:-/var/tmp/plh-gamepad/pfx}"

# ---- A. end-to-end script
# the throwaway prefix is created here if absent (slow, once), so the e2e timeout only covers the checks
if [ ! -f "$PFX/system.reg" ]; then
  info "creating the throwaway prefix $PFX (wineboot -i, once)"
  mkdir -p "$(dirname "$PFX")"; ( wine_env; WINEPREFIX="$PFX" timeout 600 "$WINE" wineboot -i >/dev/null 2>&1 )
fi
timeout 300 bash "$REPO/src/input/tests/gamepad-e2e.sh" >"$out/e2e.log" 2>&1; rc=$?
grep -E '^(ok|not ok|1\.\.|Bail)' "$out/e2e.log" | sed 's/^/  # e2e: /'
nok=$(grep -c '^ok ' "$out/e2e.log"); nbad=$(grep -c '^not ok ' "$out/e2e.log")
grep -qE '# SKIP|^1\.\.0' "$out/e2e.log" && skip "gamepad-e2e.sh skipped itself: $(grep -E 'SKIP' "$out/e2e.log" | head -1)"
grep -q "^Bail out: lock\|^Bail out! lock" "$out/e2e.log" && skip "input lock /tmp/plh-input.lock held by another injector"
grep -q '^Bail out' "$out/e2e.log" && fail "gamepad-e2e.sh bailed out: $(grep '^Bail out' "$out/e2e.log")"
[ $rc = 0 ] && [ "$nbad" = 0 ] || errs+=("A: e2e rc=$rc, $nbad failed check(s)")
[ "$nok" -ge 15 ] || errs+=("A: only $nok ok checks (expected >= 15): a check vanished?")
# Order-sensitive checks on the first evdev capture (the e2e only tests set membership: a report that never
# arrived in order, or a stuck button, would go unnoticed): A pressed before B, final state all released,
# no stuck key or axis, and the evdev events carry no value out of range.
ev=/tmp/plh-entrees-pad/evdev.log
if [ -s "$ev" ]; then
  r=$(python3 - "$ev" <<'P'
import sys
ev=[tuple(map(int,l.split()[1:])) for l in open(sys.argv[1]) if l.startswith("EV ")]
last={}; first={}
for i,(t,c,v) in enumerate(ev):
    last[(t,c)]=v; first.setdefault((t,c,v),i)
bad=[]
a=first.get((1,304,1)); b=first.get((1,305,1)); x=first.get((1,307,1))
if a is None or b is None or x is None or not (a<b<x): bad.append(f"press order A<B<X violated ({a},{b},{x})")
# ABS_Y/ABS_RY are inverted like the kernel xpad driver (~v), so a centered stick rests at -1
stuck={k:v for k,v in last.items() if v!=0 and not (k in ((3,1),(3,4)) and v==-1)}
if stuck: bad.append(f"final state not all released: {stuck}")
for t,c,v in ev:
    if t==3 and c in (0,1,3,4) and not -32768<=v<=32767: bad.append(f"axis {c} out of range {v}")
    if t==3 and c in (2,5) and not 0<=v<=255: bad.append(f"trigger {c} out of range {v}")
print("OK" if not bad else "; ".join(bad)); sys.exit(1 if bad else 0)
P
  ) || errs+=("A: evdev sequence: $r")
  info "A: evdev sequence check: $r"
else errs+=("A: no evdev capture to check ($ev)"); fi
sleep 1
[ "$(pads)" = "$pad0" ] || errs+=("A: virtual pad left behind after the e2e run ($pad0 -> $(pads))")
info "A: e2e $nok ok / $nbad not ok"

# ---- B and C: own bridge + relay in the throwaway prefix
exec 8>/tmp/plh-input.lock
flock -w 120 8 || skip "input lock /tmp/plh-input.lock held by another injector"
wine_env
export WINEPREFIX="$PFX" WINEDEBUG=-all
[ -f "$PFX/system.reg" ] || fail "throwaway prefix $PFX missing (gamepad-e2e.sh creates it)"
export PLH_INPUT_BACKEND=uinput
cleanup() {
  for p in $(pgrep -f 'plh-pad-relay.exe|padprobe.exe|pad_malformed.exe'); do
    { tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qx "WINEPREFIX=$PFX" && kill -9 "$p" 2>/dev/null
  done
  "$WINESERVER" -k 2>/dev/null; stop_bridge
}
trap cleanup EXIT
start_bridge "$TMPD/pad-rt" --capture-backend test
timeout 200 "$WINE" "$B/input/pe/plh-pad-relay.exe" --install "$B/input/pe/plh-vigem.sys" --seconds 160 -v >"$out/relay.log" 2>&1 &
rp=$!
for _ in $(seq 150); do grep -q READY "$out/relay.log" 2>/dev/null && break; sleep 0.2; done
grep -q READY "$out/relay.log" || fail "relay not READY: $(tail -2 "$out/relay.log" | tr '\n' ' ')"
relay_pid() { for p in $(pgrep -f 'plh-pad-relay.exe'); do { tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qx "WINEPREFIX=$PFX" && echo "$p"; done; }
[ -n "$(relay_pid)" ] || fail "relay process not found"

# B. malformed IOCTLs
timeout 120 "$WINE" "$B/tests/pe/pad_malformed.exe" >"$out/mal.log" 2>&1 & mp=$!
# the evdev watcher starts at the final cycle (the hostile phase may create/remove pads of its own)
for _ in $(seq 600); do grep -q '^final_version' "$out/mal.log" 2>/dev/null && break; sleep 0.2; done
"$B/input/pad-evdev-dump" 20 20 >"$out/evB.log" 2>&1 & dp=$!
wait $mp; mrc=$?
tr -d '\r' <"$out/mal.log" | sed 's/^/  # malformed: /'
wait $dp 2>/dev/null
mal() { tr -d '\r' <"$out/mal.log" | grep "^$1: " | head -1; }
want_fail() { mal "$1" | grep -qE ': fail' || errs+=("B: $1 should be refused, got '$(mal "$1")'"); }
want_ok()   { mal "$1" | grep -qE ': ok'   || errs+=("B: $1 should succeed, got '$(mal "$1")'"); }
[ $mrc = 0 ] || errs+=("B: pad_malformed.exe rc=$mrc (crash/timeout?)")
for c in plug_len0 plug_len4 plug_len8_no_type plug_type_ds4 plug_type_garbage plug_duplicate_serial \
         unplug_unknown_serial unplug_len4 unplug_len0 submit_unknown_serial devtype_disk devtype_zero code_ffffffff; do want_fail $c; done
want_ok plug_first
want_ok plug_serial_8; want_ok plug_serial_9; want_ok plug_serial_10          # 4 slots: serials 7..10
for c in plug_serial_11 plug_serial_12 plug_serial_13; do want_fail $c; done  # slot exhaustion refused
tr -d '\r' <"$out/mal.log" | grep -q '^fuzz: 300 requests' || errs+=("B: fuzz did not complete")
tr -d '\r' <"$out/mal.log" | grep -q '^final cycle: ok' || errs+=("B: driver broken after the hostile requests (final cycle not ok)")
grep -qx 'EV 1 304 1' "$out/evB.log" || errs+=("B: the final A-button report never reached evdev (pad chain broken after hostile requests)")
grep -qx REMOVED "$out/evB.log" || errs+=("B: no pad removal seen")
[ -n "$(relay_pid)" ] || errs+=("B: relay died during the hostile requests")
sleep 2
[ "$(pads)" = "$pad0" ] || errs+=("B: virtual pad leaked after the hostile client exited ($pad0 -> $(pads))")

# C. relay SIGKILLed while the client stays plugged
if [ -n "$(relay_pid)" ]; then
  "$B/input/pad-evdev-dump" 30 40 >"$out/evC.log" 2>&1 & dp=$!
  timeout 100 "$WINE" "$B/input/pe/padprobe.exe" 3000 leave >"$out/probeC.log" 2>&1 &
  pp=$!
  for _ in $(seq 150); do grep -q '^FOUND' "$out/evC.log" && break; sleep 0.1; done
  grep -q '^FOUND' "$out/evC.log" || errs+=("C: pad never appeared")
  sleep 0.5
  client_alive() { for p in $(pgrep -f 'padprobe.exe'); do { tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qx "WINEPREFIX=$PFX" && return 0; done; return 1; }
  client_alive || errs+=("C: client already gone before the relay kill (test void)")
  kill -9 $(relay_pid) 2>/dev/null
  for _ in $(seq 40); do grep -qx REMOVED "$out/evC.log" && break; sleep 0.1; done
  if grep -qx REMOVED "$out/evC.log"; then
    client_alive && info "C: pad removed while the client was still alive" || errs+=("C: client had already exited: removal not attributable to the relay kill")
  else errs+=("C: pad still present 4 s after the relay was killed (leaked uinput device)"); fi
  kill $dp 2>/dev/null; wait $dp 2>/dev/null
else errs+=("C: not run (relay gone)"); fi
cleanup; trap - EXIT
sleep 1
[ "$(pads)" = "$pad0" ] || errs+=("C: virtual pad leaked at the end ($pad0 -> $(pads))")
if [ ${#errs[@]} -gt 0 ]; then printf '  # - %s\n' "${errs[@]}"; fail "${#errs[@]} discrepancy(ies): ${errs[0]}"; fi
pass "gamepad: e2e $nok checks ok, hostile IOCTLs refused without damage, no pad leaked after a killed relay"
