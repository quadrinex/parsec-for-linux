#!/usr/bin/env bash
# Native chain + bridge primitives (AF_UNIX, memfd, SCM_RIGHTS).
. "$(dirname "$0")/lib/common.sh"
b="$BUILD/native/basic"; [ -x "$b" ] || fail "native binary not built: $b"
out=$("$b"); rc=$?; echo "$out" | sed 's/^/  # /'
[ $rc = 0 ] || fail "basic native test"
pass "basic native test"
