#!/usr/bin/env bash
# PE chain (mingw-w64): an .exe built here runs under Wine in runtime/prefix;
# records the screen layout seen by Windows (reference for T2/T4).
. "$(dirname "$0")/lib/common.sh"
need_cmd x86_64-w64-mingw32-gcc
exe=$(pe_bin hello) || exit $?
need_prefix
out=$(run_pe "$exe" 2>"$TMPD/hello.err"); rc=$?
echo "$out" | sed 's/^/  # /'
[ $rc = 0 ] || fail "hello.exe rc=$rc ($(tail -3 "$TMPD/hello.err"))"
pass "hello.exe runs under $(echo "$out" | sed -n 's/^wine_version=//p')"
