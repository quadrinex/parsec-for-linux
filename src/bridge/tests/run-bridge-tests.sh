#!/usr/bin/env bash
# run-bridge-tests.sh — TAP tests of the bridge. Owner: capture.
# usage: run-bridge-tests.sh <build/bridge directory>
# Starts a private plh-bridge (temporary runtime directory, test-pattern backend), then checks:
#  native ABI == PE ABI, native client (unix), PE client under Wine (TCP+token, shm via Z:),
#  wrong token refused. Wine: throwaway prefix /tmp/plh-capture/wpfx-test unless WINEPREFIX is given.
set -u
B=${1:?build directory}
T=$(mktemp -d /tmp/plh-bridge-test.XXXXXX)
RD=$T/rt; mkdir -m 700 "$RD"
export WINEPREFIX=${WINEPREFIX:-/tmp/plh-capture/wpfx-test} WINEDEBUG=-all
n=0; fail=0
ok()  { n=$((n+1)); echo "ok $n - $1"; }
nok() { n=$((n+1)); fail=$((fail+1)); echo "not ok $n - $1"; [ $# -gt 1 ] && sed 's/^/# /' "$2"; }
cleanup() { [ -n "${BP:-}" ] && kill "$BP" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
echo "1..7"

if diff <("$B/proto-sizes") <(wine "$B/proto-sizes.exe" 2>/dev/null | tr -d '\r') > "$T/abi.diff"; then ok "protocol ABI identical native/PE"; else nok "protocol ABI identical native/PE" "$T/abi.diff"; fi

PLH_INPUT_BACKEND=none "$B/plh-bridge" --runtime-dir "$RD" --capture-backend test > "$T/bridge.log" 2>&1 & BP=$!
for _ in $(seq 50); do [ -S "$RD/bridge.sock" ] && [ -s "$RD/port" ] && break; sleep 0.1; done
if [ "$(stat -c %a "$RD/token")" = 600 ] && [ "$(stat -c %s "$RD/token")" = 32 ]; then ok "32-byte token, mode 0600"; else nok "32-byte token, mode 0600"; fi

export PLH_BRIDGE_DIR=$RD
if "$B/plh-captest" -b test -n 120 -q > "$T/nat.log" 2>&1 && [ "$(grep -c '^RESULT.*counter_errors=0' "$T/nat.log")" -ge 1 ]; then ok "native unix client: test-pattern frames received, exact counters"; else nok "native unix client" "$T/nat.log"; fi

if PLH_BRIDGE_FORCE_TCP=1 "$B/plh-captest" -b test -n 60 -q > "$T/tcp.log" 2>&1; then ok "native tcp+token client"; else nok "native tcp+token client" "$T/tcp.log"; fi

mkdir -m 700 "$T/bad"; cp "$RD/port" "$T/bad/"; head -c 32 /dev/urandom > "$T/bad/token"
if PLH_BRIDGE_DIR=$T/bad PLH_BRIDGE_FORCE_TCP=1 "$B/plh-captest" -b test -n 1 -q > "$T/bad.log" 2>&1; then nok "wrong token refused" "$T/bad.log"
elif grep -q ": -3" "$T/bad.log"; then ok "wrong token refused (PLH_E_AUTH)"; else nok "wrong token refused" "$T/bad.log"; fi

[ -d "$WINEPREFIX" ] || wineboot -i > /dev/null 2>&1
if timeout 120 wine "$B/plh-captest.exe" -b test -n 120 -q > "$T/pe.log" 2>&1 && grep -q "connected (tcp)" "$T/pe.log" \
   && [ "$(grep -c '^RESULT.*counter_errors=0' "$T/pe.log")" -ge 1 ]; then ok "PE client under Wine: TCP + shm Z:\\dev\\shm, exact frames"
else nok "PE client under Wine" "$T/pe.log"; fi
grep '^RESULT' "$T/pe.log" | sed 's/^/# PE /'
if "$B/test-token" > "$T/token.log" 2>&1; then ok "portal token: mode 0600, symlink refused"; else nok "portal token" "$T/token.log"; fi
exit $((fail > 0))
