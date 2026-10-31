#!/usr/bin/env bash
# Decision logic of bin/parsec-linux-host watch_screens / client_connected, with stubs only:
# no real screen, no xrandr, no host, no pkill. Functions are extracted from the script itself.
. "$(dirname "$0")/lib/common.sh"
HOST="$REPO/bin/parsec-linux-host"
[ -f "$HOST" ] || skip "bin/parsec-linux-host missing"
grep -q '^screens_changed()' "$HOST" || skip "screens_changed not present yet"
D="$TMPD/t04"; rm -rf "$D"; mkdir -p "$D"

# Extract the three functions; replace the final exec by a marker so the loop is observable.
fn() { awk -v n="$1" '$0 ~ "^"n"\\(\\) \\{" {p=1} p {print} p && /^}/ {exit}' "$HOST"; }
{ fn client_connected; fn screens_changed; fn watch_screens; } | sed 's|exec "\$0" restart|echo RESTART; return 0|' >"$D/fns.sh"
grep -q RESTART "$D/fns.sh" || fail "could not extract watch_screens (exec \"\$0\" restart not found)"

# run_case <layout-now> <layout-saved> <client log content or NOLOG> <max sleeps> -> prints events
# env: AGE = age in s of the saved layout file (default 600), RUNNING = 1|0 (parsecd stub, default 1)
run_case() {
  ( set -u
    PLH_TRACES="$D/traces"; mkdir -p "$PLH_TRACES"; printf %s "$2" >"$PLH_TRACES/screen-layout"; touch -d "-${AGE:-600} seconds" "$PLH_TRACES/screen-layout"
    LOG="$D/parsec.log"; rm -f "$LOG"; [ "$3" = NOLOG ] || printf '%b' "$3" >"$LOG"
    running() { [ "${RUNNING:-1}" = 1 ]; }
    SAVED="$2"; NOW="$1"; N=0; LIMIT=$4
    screen_layout() { printf %s "$NOW"; }
    wait_for_screens() { echo WAIT; }
    sleep() { N=$((N+1)); [ $N -gt "$LIMIT" ] && { echo TIMEOUT; exit 0; }
              [ -n "${DISCONNECT_AT:-}" ] && [ $N -eq "$DISCONNECT_AT" ] && { printf '2026 D parsec: disconnected.\r\n' >>"$LOG"; [ -n "${FLIPBACK:-}" ] && NOW="$SAVED"; }; return 0; }
    . "$D/fns.sh"
    watch_screens ) 2>&1 | tr '\n' ' '
}
L3='0: +*DP-2 A
 1: +HDMI-A-1 B'
L2='0: +*DP-2 A'
CONN='x client: connected.\r\n'; DISC='x client: connected.\r\nx client: disconnected.\r\n'

r=$(run_case "$L3" "$L3" NOLOG 5);   [ "$r" = "TIMEOUT " ] || fail "unchanged layout must never restart ($r)"
info "unchanged layout: no restart"
r=$(run_case "$L2" "$L3" "$DISC" 5); case $r in "WAIT RESTART "*) ;; *) fail "unplug, no client: expected restart ($r)";; esac
info "layout changed, no client: restart"
r=$(run_case "$L2" "$L3" NOLOG 5);   case $r in "WAIT RESTART "*) ;; *) fail "missing/rotated log counts as no client: expected restart ($r)";; esac
info "missing log is treated as 'no client' (restart allowed; see findings)"
r=$(run_case "$L2" "$L3" "$CONN" 6); case $r in *RESTART*) fail "restarted with a client connected ($r)";; esac
info "layout changed, client connected: no restart while connected"
r=$(DISCONNECT_AT=3 run_case "$L2" "$L3" "$CONN" 10); case $r in *RESTART*) ;; *) fail "no restart after the client left ($r)";; esac
info "restart happens once the client disconnects"
# Stale "connected." line (parsecd crashed, log not closed): not a client, restart must go ahead.
r=$(RUNNING=0 run_case "$L2" "$L3" "$CONN" 5); case $r in *RESTART*) ;; *) fail "stale connected line with parsecd dead blocks the restart ($r)";; esac
info "stale connected line + parsecd not running: restart"
# Flapping: last restart (layout file mtime) under 60 s ago => no restart, no loop.
r=$(AGE=10 run_case "$L2" "$L3" "$DISC" 8); case $r in *RESTART*) fail "restart less than 60 s after the previous one ($r)";; esac
info "layout changed 10 s after the last start: no restart"
# Recheck after the client leaves: layout back to the saved one => no restart.
r=$(DISCONNECT_AT=3 FLIPBACK=1 run_case "$L2" "$L3" "$CONN" 10); case $r in *RESTART*) fail "restarted although the layout was back to the saved one";; esac
info "layout restored while the client was connected: no restart"
# Layout flapping back during the wait: re-check must cancel the restart.
r=$( ( NOW=x; run_case "$L3" "$L3" NOLOG 3 ) ); case $r in *RESTART*) fail "restart without any change";; esac
# xrandr (and kscreen-doctor) failing => empty layout, e.g. while the session ends: must NOT restart.
r=$(run_case "" "$L3" "$DISC" 5)
case $r in *RESTART*) fail "empty screen_layout (xrandr failing) triggers a restart: the host would restart into a dead session";; esac
info "empty layout: no restart"
# The pkill/pgrep pattern must match the watcher cmdline and not itself nor the restart command.
pat='parsec-linux-host[ ]watch'
echo "bash /x/bin/parsec-linux-host watch" | grep -qE "$pat" || fail "pattern does not match the watcher"
echo "bash /x/bin/parsec-linux-host restart" | grep -qE "$pat" && fail "pattern matches the restart command (watcher after exec)"
echo "pkill -f $pat" | grep -qE "$pat" && fail "pattern matches its own pkill command line"
pass "watch_screens decision logic (restart / wait for client / no change / empty layout) and pkill pattern"
