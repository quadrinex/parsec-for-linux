#!/usr/bin/env bash
# T4 — Input injected through the Windows APIs (absolute SendInput, SetCursorPos,
# key, wheel) from the Wine prefix, received by a real Wayland window
# (tools/input_sink.py) at the exact coordinates on each screen.
#
# Safety rules (CLAUDE.md, incident of 2026-10-30) applied here:
#  - no target within 50 px of an OUTER desktop edge (checked before
#    any injection); the junctions between screens are tested (inner edges);
#  - first a probe click at the center of the primary screen: if it is not received
#    by our window, abort at once with no further injection;
#  - then one point at a time, each verified received before the next;
#  - focus verified on our window before each click and before the key;
#    only key: F24;
#  - one injector at a time: flock /tmp/plh-input.lock held for the whole test;
#  - real KWin cursor position read before/after and tracked during the test
#    (tools/kwin_cursor.sh): shows whether winex11 moves the real pointer (XWarpPointer)
#    even when nothing reaches the window.
. "$(dirname "$0")/lib/common.sh"
need_cmd kscreen-doctor python3
python3 -c 'import PySide6' 2>/dev/null || skip "PySide6 absent"
exe=$(pe_bin sendinput_probe) || exit $?
hello=$(pe_bin hello) || exit $?
need_prefix
need_intrusive "full-screen windows + injected clicks/key ~20 s"
need_cmd flock qdbus6 journalctl
no_client_connected
[ -x "$REPO/build/input/pe/plh-input-hook.exe" ] || fail "plh-input-hook.exe not built (make -C src/input)"
exec 8>/tmp/plh-input.lock
flock -w 120 8 || skip "verrou /tmp/plh-input.lock tenu par un autre injecteur depuis 120 s"
out="$TMPD/input"; rm -rf "$out"; mkdir -p "$out"; hp=
KC="$TESTS_DIR/tools/kwin_cursor.sh"
map=$(run_pe "$hello" 2>/dev/null | python3 "$TESTS_DIR/tools/layout.py") || fail "Windows layout ≠ KWin (see t03)"
read -r dx dy < <(echo "$map" | sed -n 's/^translation //p' | tr ',' ' ')
# Targets in KWin coordinates: probe (center of the primary), then per screen an
# inner point, and the two pixels of each junction between neighboring screens.
mapfile -t targets < <(kscreen-doctor -j | python3 -c '
import json,sys
o=sorted([o for o in json.load(sys.stdin)["outputs"] if o["enabled"]], key=lambda o:o["pos"]["x"])
r=[(x["name"],x["pos"]["x"],x["pos"]["y"],x["size"]["width"],x["size"]["height"],x.get("priority")) for x in o]
X0=min(a[1] for a in r); X1=max(a[1]+a[3] for a in r)-1; Y0=min(a[2] for a in r); Y1=max(a[2]+a[4] for a in r)-1
print("bbox",X0,Y0,X1,Y1)
p=[a for a in r if a[5]==1] or r[:1]
n,x,y,w,h,_=p[0]; print(n,x+w//2,y+h//2)
for n,x,y,w,h,_ in r: print(n,x+333,y+444)
for (n,x,y,w,h,_),(n2,x2,y2,w2,h2,_) in zip(r,r[1:]):
    if x+w==x2: print(n,x+w-1,y+h//2+111); print(n2,x2,y2+h2//2+111)')
read -r _ X0 Y0 X1 Y1 <<<"${targets[0]}"; targets=("${targets[@]:1}")
for t in "${targets[@]}"; do set -- $t
  if [ $2 -lt $((X0+50)) ] || [ $2 -gt $((X1-50)) ] || [ $3 -lt $((Y0+50)) ] || [ $3 -gt $((Y1-50)) ]; then
    fail "target $2,$3 too close to an outer edge ($X0,$Y0,$X1,$Y1) — refused before injection"; fi
done
cur0=$("$KC" pos) || cur0="?"
info "KWin cursor before: $cur0"
mon_since=$("$KC" mon-start t30)
python3 "$TESTS_DIR/tools/input_sink.py" --seconds 40 >"$out/sink.log" 2>"$out/sink.err" &
sp=$!
cleanup() { kill $hp 2>/dev/null; pkill -f "plh-input-hook.exe --seconds" 2>/dev/null; stop_bridge; kill $sp 2>/dev/null; wait $sp 2>/dev/null; [ -n "${mon_since:-}" ] && "$KC" mon-stop t30 "$mon_since" >"$out/kwin-mon.log" 2>/dev/null; mon_since=""; }
trap cleanup EXIT
for _ in $(seq 50); do [ "$(grep -c '^ready' "$out/sink.log")" -ge 3 ] && break; sleep 0.1; done
sleep 1.5
# Chain tested = the host's: SendInput (Wine) -> plh-input-hook.exe -> private plh-bridge (uinput backend) -> KWin.
# Started and stopped by this test; the hook stops by itself after 90 s at most.
hp=
export PLH_INPUT_BACKEND=uinput PLH_INPUT_MOUSE_MODE=abs
start_bridge "$TMPD/input-rt" --capture-backend kwin
wine_env
timeout 90 "$WINE" "$REPO/build/input/pe/plh-input-hook.exe" --seconds 80 >"$out/hook.log" 2>&1 &
hp=$!
for _ in $(seq 100); do grep -q READY "$out/hook.log" 2>/dev/null && break; sleep 0.1; done
grep -q READY "$out/hook.log" || fail "plh-input-hook.exe is not ready: $(tr -d '\r' <"$out/hook.log" | tail -2 | tr '\n' ' ')"
sleep 1
# Positive control (native, entrees' uinput, without touching the KWin config: --no-flat):
# a click at the center of the primary must be seen by the receiver AND by the KWin monitor.
# Otherwise the harness itself proves nothing: FAIL. This click also gives focus to the receiver.
CTL="$REPO/build/input/plh-input-ctl"
set -- ${targets[0]}
if [ -x "$CTL" ] && [ -w /dev/uinput ]; then
  # Step by step through a FIFO: move, check the REAL KWin cursor, and only
  # then the click. Any off-target position during the control → abort and FAIL.
  rm -f "$out/ctl.fifo"; mkfifo "$out/ctl.fifo"
  "$CTL" --no-flat --desktop "$X0,$Y0,$((X1-X0+1))x$((Y1-Y0+1))" --settle 700 <"$out/ctl.fifo" >"$out/ctl.log" 2>&1 &
  cp=$!; exec 7>"$out/ctl.fifo"
  echo "abs $2 $3" >&7; sleep 1.2
  pos=$("$KC" pos || echo "?")
  if [ "$pos" != "$2 $3" ]; then exec 7>&-; wait $cp
    fail "positive control: after "abs $2 $3" the KWin cursor is at $pos — no click sent (entrees' injection tool at fault)"; fi
  printf 'btn left click\nsleep 300\n' >&7; exec 7>&-; wait $cp
  pos=$("$KC" pos || echo "?")
  [ "$pos" = "$2 $3" ] || fail "positive control: the click moved the KWin cursor from $2,$3 to $pos (entrees' injection tool at fault)"
  for _ in $(seq 20); do grep -q "^press .* global=$2,$3 button=1" "$out/sink.log" && break; sleep 0.1; done
  grep -q "^press .* global=$2,$3 button=1" "$out/sink.log" || fail "positive control: uinput click at $2,$3 not seen by the receiver ($(tail -1 "$out/sink.log")) — harness unreliable"
  ctl_ok=1; info "positive control: native uinput click received at $2,$3, KWin cursor stayed on the target"
  grep -c . "$out/sink.log" >"$out/sink.ctl"
else
  ctl_ok=0; info "no positive control (plh-input-ctl missing or /dev/uinput not accessible)"
fi
# Focus = ACTIVE KWin window (KWin script): Qt focusInEvent is not emitted here, we do not rely on it.
has_focus() { case "$("$TESTS_DIR/tools/kwin_active.sh")" in plh-input-sink*) return 0;; *) return 1;; esac; }
skipn=$(cat "$out/sink.ctl" 2>/dev/null || echo 0)
after_ctl() { tail -n +"$((skipn+1))" "$out/sink.log"; }
wait_line() { # pattern, delay in tenths of a second — only in what follows the positive control
  for _ in $(seq "$2"); do after_ctl | grep -qE "$1" && return 0; sleep 0.1; done; return 1; }
errs=(); n=0
for t in "${targets[@]}"; do set -- $t
  n=$((n+1))
  if [ $n -gt 1 ] && ! has_focus; then errs+=("focus lost before point $2,$3: stop"); break; fi
  wx=$(( $2 + dx )); wy=$(( $3 + dy ))
  run_pe "$exe" click $wx $wy >>"$out/click.log" 2>&1
  if wait_line "^press $1 local=.* global=$2,$3 button=1" 15; then
    info "click received at $2,$3 ($1) [Windows $wx,$wy]"
  else
    got=$(after_ctl | grep '^press' | tail -1)
    errs+=("SendInput click not received at $2,$3 ($1); last received: ${got:-none}")
    [ $n = 1 ] && { info "probe click not received: aborted without further injection"; break; }
    break
  fi
done
if [ ${#errs[@]} = 0 ]; then
  set -- ${targets[0]}
  run_pe "$exe" setpos $(( $2 + dx )) $(( $3 + dy )) >"$out/setpos.log" 2>&1
  [ "$(after_ctl | grep -c "global=$2,$3 button=1")" -ge 2 ] || { sleep 1; [ "$(after_ctl | grep -c "global=$2,$3 button=1")" -ge 2 ]; } || info "known limitation D-015: SetCursorPos+mouse_event click not relayed (LL hooks do not see SetCursorPos); informational measurement, not part of the verdict"
  if has_focus; then
    run_pe "$exe" key 0x87 >"$out/key.log" 2>&1
    wait_line '^key release key=16777287' 15 || errs+=("F24 key (press+release) not received")   # Qt::Key_F24 = 0x01000047
  else errs+=("focus lost before the key: F24 not sent"); fi
  run_pe "$exe" wheel -120 >"$out/wheel.log" 2>&1
  wait_line '^wheel .* delta=-120' 15 || errs+=("wheel -120 not received")
fi
cleanup; trap - EXIT
cur1=$("$KC" pos) || cur1="?"
info "KWin cursor after: $cur1; KWin positions during the test: $(wc -l <"$out/kwin-mon.log")"
warped=0
for t in "${targets[@]}"; do set -- $t; [ $ctl_ok = 1 ] && [ "$2 $3" = "$(echo "${targets[0]}" | cut -d' ' -f2-)" ] && continue  # control target
  grep -qx "$2 $3" "$out/kwin-mon.log" && { warped=$((warped+1)); info "the real KWin cursor went exactly through target $2,$3"; }; done
if [ $ctl_ok = 1 ] && [ "$cur0" != "$(echo "${targets[0]}" | cut -d" " -f2-)" ] && ! grep -qx "$(echo "${targets[0]}" | cut -d' ' -f2-)" "$out/kwin-mon.log"; then
  fail "positive control: the KWin monitor did not see the uinput cursor — cursor tracking unreliable"; fi
if [ $warped -gt 0 ]; then info "=> the real pointer moves ($warped/${#targets[@]} targets reached by the KWin cursor)"
elif [ "$cur0" = "$cur1" ]; then info "=> the real pointer did not move: Wine injection stays internal (no effective XWarpPointer)"
else info "=> KWin cursor moved ($cur0 → $cur1) without going through a target: probable human activity, inconclusive"; fi
info "--- receiver log"; sed 's/^/  #   /' "$out/sink.log"
if [ ${#errs[@]} -gt 0 ]; then printf '  # - %s\n' "${errs[@]}"; fail_or_skip I3 "${#errs[@]} discrepancy(ies) : ${errs[0]}"; fi
pass "${#targets[@]} exact clicks (junctions between screens included), F24 and wheel received"
