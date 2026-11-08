#!/usr/bin/env bash
# S5 live test: does Wine follow a KWin virtual output appearing/disappearing without a restart?
# Needs no client. Stops and restarts the host (under /tmp/plh-host.lock, fd closed for daemons).
# Output: runtime/traces/s5-displaywatch.txt (probe), runtime/traces/s5-x11drv.log (+x11drv).
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../../.." && pwd)"
if [ -z "${PLH_HOST_LOCK_HELD:-}" ]; then
    export PLH_HOST_LOCK_HELD=1
    flock -n -E 99 -o /tmp/plh-host.lock "$0" "$@"; rc=$?
    [ $rc -eq 99 ] && echo "host lock held, try later" >&2
    exit $rc
fi
. "$ROOT/runtime/env.sh"
HOST="$ROOT/bin/parsec-linux-host"
"$HOST" status | grep -q '^client .*connected$' && { echo "client connected: abort" >&2; exit 1; }
OUT="$PLH_TRACES"
"$HOST" stop; sleep 2
cleanup() { "$WINESERVER" -k 2>/dev/null; sleep 1; "$HOST" start; "$HOST" status; }
trap cleanup EXIT
WINEDEBUG=+x11drv "$WINE" "$ROOT/build/runtime/displaywatch.exe" 25 >"$OUT/s5-displaywatch.txt" 2>"$OUT/s5-x11drv.log" </dev/null &
sleep 8
echo "xrandr before:"; xrandr --listmonitors
"$ROOT/build/bridge/vout-probe" -t 8 >"$OUT/s5-voutprobe.txt" 2>&1 </dev/null &
sleep 4; echo "xrandr during:"; xrandr --listmonitors
wait
echo "xrandr after:"; xrandr --listmonitors
cat "$OUT/s5-displaywatch.txt"
