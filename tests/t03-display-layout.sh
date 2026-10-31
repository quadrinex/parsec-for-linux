#!/usr/bin/env bash
# Screen layout seen by Windows (hence by Parsec) under Wine in the dedicated
# prefix = KWin layout up to a translation (Windows puts the primary at
# 0,0); same primary screen. Basis of the T2/T4 coordinates.
. "$(dirname "$0")/lib/common.sh"
need_cmd kscreen-doctor
exe=$(pe_bin hello) || exit $?
need_prefix
out=$(run_pe "$exe" 2>/dev/null) || fail "hello.exe failed"
echo "$out" | grep -E '^(monitor|virtual)' | sed 's/^/  # /'
res=$(echo "$out" | python3 "$TESTS_DIR/tools/layout.py"); rc=$?
echo "$res" | sed 's/^/  # /'
[ $rc = 0 ] || fail "Windows layout ≠ KWin layout"
pass "3 Windows screens = translated KWin layout ($(echo "$res" | sed -n 's/^translation //p'))"
