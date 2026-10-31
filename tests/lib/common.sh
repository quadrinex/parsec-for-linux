# shellcheck shell=bash
# Common test functions (sourced by every tests/tNN-*.sh).
# Return code convention (as in automake):
#   0  = PASS, 1 = FAIL, 77 = SKIP (missing component / prerequisite).
# Never PASS by default: a test that verified nothing must end in SKIP.

set -u
TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$TESTS_DIR/.." && pwd)"
BUILD="$REPO/build/tests"
# Prefix: PLH_PREFIX (throwaway, exploration), else the one from runtime/env.sh (WINEPREFIX, PLH_DATA_DIR), else runtime/prefix.
_envpfx() { [ -f "$REPO/runtime/env.sh" ] && ( . "$REPO/runtime/env.sh" >/dev/null 2>&1; printf %s "${WINEPREFIX:-}" ); }
PREFIX_DIR="${PLH_PREFIX:-$(_envpfx)}"; PREFIX_DIR="${PREFIX_DIR:-$REPO/runtime/prefix}"  # PLH_PREFIX: throwaway prefix, exploration only
TMPD="/tmp/plh-critique"
mkdir -p "$TMPD"

info() { echo "  # $*"; }
pass() { echo "  # PASS: $*"; exit 0; }
fail() { echo "  # FAIL: $*"; exit 1; }
skip() { echo "  # SKIP: $*"; exit 77; }

need_cmd() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 || skip "command not found: $c"; done; }

# Tests that display something, inject input or play sound in the real
# session: only when PLH_TEST_INTRUSIVE=1.
need_intrusive() {
  [ "${PLH_TEST_INTRUSIVE:-0}" = 1 ] || skip "intrusive test ($*): rerun with PLH_TEST_INTRUSIVE=1"
}

# All components are delivered: a failure is a FAIL, never a SKIP.
fail_or_skip() { # $1 = component id, $2 = message
  fail "$2 (component $1)"
}

need_prefix() {
  [ -f "$PREFIX_DIR/system.reg" ] || skip "dedicated Wine prefix missing ($PREFIX_DIR) — runtime/setup-prefix.sh"
}

# Wine environment identical to Parsec's: runtime/env.sh is authoritative
# (isolated wine-staging, WINEDLLOVERRIDES); runtime/run-env.sh (project DLL
# overrides) if present. Without runtime/env.sh: system wine.
wine_env() {
  if [ -f "$REPO/runtime/env.sh" ]; then
    # shellcheck disable=SC1091
    . "$REPO/runtime/env.sh"
  fi
  if [ -f "$REPO/runtime/run-env.sh" ]; then
    # shellcheck disable=SC1091
    . "$REPO/runtime/run-env.sh"
  fi
  export WINEPREFIX="$PREFIX_DIR"
  export WINEDEBUG="${WINEDEBUG:--all}"
  WINE="${WINE:-wine}"
}

run_pe() { # run_pe <exe> [args...] — program's stdout, without the \r (Windows CRLF)
  wine_env
  timeout "${PE_TIMEOUT:-60}" "$WINE" "$@" | tr -d '\r'
  return "${PIPESTATUS[0]}"
}

pe_bin() { # pe_bin <name>: path of the built .exe, SKIP if missing
  local f="$BUILD/pe/$1.exe"
  [ -f "$f" ] || skip "PE test program not built: $f (make -C tests build)"
  echo "$f"
}

# PRIVATE plh-bridge daemon (temporary runtime dir, never the user's), started and stopped by the test.
# start_bridge <dir> [plh-bridge args...]; exports PLH_BRIDGE_DIR, BRIDGE_PID. Environment passed as is (e.g. PLH_INPUT_BACKEND).
start_bridge() {
  local d=$1; shift
  [ -x "$REPO/build/bridge/plh-bridge" ] || fail "plh-bridge not built (make -C src/bridge)"
  rm -rf "$d"; mkdir -p "$d"; chmod 700 "$d"
  "$REPO/build/bridge/plh-bridge" --runtime-dir "$d" "$@" >"$d/../bridge-$(basename "$d").log" 2>&1 &
  BRIDGE_PID=$!
  for _ in $(seq 50); do [ -s "$d/port" ] && break; sleep 0.1; done
  [ -s "$d/port" ] || { kill $BRIDGE_PID 2>/dev/null; fail "private plh-bridge not started ($(tail -2 "$d/../bridge-$(basename "$d").log" | tr '\n' ' '))"; }
  export PLH_BRIDGE_DIR=$d
}
stop_bridge() { [ -n "${BRIDGE_PID:-}" ] && { kill "$BRIDGE_PID" 2>/dev/null; wait "$BRIDGE_PID" 2>/dev/null; }; BRIDGE_PID=; }
# Before any test that touches the session: no Parsec client connected (so we do not disturb it).
no_client_connected() {
  pgrep -f 'parsecd\.exe' >/dev/null || return 0
  local log="$PREFIX_DIR/drive_c/users/$USER/AppData/Roaming/Parsec/log.txt"
  [ -f "$log" ] && tr -d '\r' <"$log" | grep -aE ' (dis)?connected\.$' | tail -1 | grep -q ' connected\.$' && skip "Parsec client connected: intrusive test refused"
  return 0
}
