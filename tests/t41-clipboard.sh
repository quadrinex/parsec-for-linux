#!/usr/bin/env bash
# T5 — Bidirectional clipboard Wayland (KDE, via Klipper D-Bus) ↔ Windows
# (OpenClipboard/GetClipboardData/SetClipboardData under Wine). The contents of the
# user's clipboard are saved then restored.
. "$(dirname "$0")/lib/common.sh"
need_cmd qdbus6
exe=$(pe_bin clipboard_probe) || exit $?
need_prefix
need_intrusive "briefly overwrites the clipboard (restored afterwards)"
K="qdbus6 org.kde.klipper /klipper"
$K getClipboardContents >/dev/null 2>&1 || skip "Klipper unavailable on D-Bus"
saved=$($K getClipboardContents)
# The host's gateway (src/clipboard/) started and stopped by the test: plh-clipd.sh (Linux) + plh-clipsync.exe (PE).
need_cmd x86_64-w64-mingw32-gcc
no_client_connected
cs="$TMPD/plh-clipsync.exe"
x86_64-w64-mingw32-gcc -std=c11 -include string.h -o "$cs" "$REPO/src/clipboard/plh-clipsync.c" || fail "compiling plh-clipsync.c"
# private directory: never touches the host's gateway if it is running
prv="$TMPD/xdg-rt"; rm -rf "$prv"; mkdir -m 700 -p "$prv"; cd_dir="$prv/plh-bridge/clip"
wine_env
restore() { kill $p1 $p2 2>/dev/null; pkill -f "plh-clipsync.exe .*--seconds" 2>/dev/null; wait $p1 $p2 2>/dev/null; $K setClipboardContents "$saved" >/dev/null 2>&1; rm -rf "$prv"; }
trap restore EXIT
XDG_RUNTIME_DIR="$prv" DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}" "$REPO/src/clipboard/plh-clipd.sh" 40 >/dev/null 2>&1 & p2=$!
timeout 45 "$WINE" "$cs" "$cd_dir" --seconds 40 >/dev/null 2>&1 & p1=$!
sleep 4
errs=()
tok1="plh-l2w-$RANDOM$RANDOM-"$'\xc3\xa9\xe2\x82\xac'   # UTF-8: e-acute, euro sign
$K setClipboardContents "$tok1"; sleep 2
got=$(run_pe "$exe" get 2>/dev/null | sed -n 's/^text=//p')
info "Linux→Windows: set \"$tok1\", read \"$got\""
[ "$got" = "$tok1" ] || errs+=("Linux→Windows: read \"$got\"")
tok2="plh-w2l-$RANDOM$RANDOM-"$'\xc3\xbc'   # UTF-8: u-umlaut
run_pe "$exe" set "$tok2" 6000 >/dev/null 2>&1 &
wp=$!; sleep 3
got2=$($K getClipboardContents)
wait $wp
info "Windows→Linux: set \"$tok2\", read \"$got2\""
[ "$got2" = "$tok2" ] || errs+=("Windows→Linux: read \"$got2\"")
if [ ${#errs[@]} -gt 0 ]; then fail_or_skip E5 "${errs[*]}"; fi
pass "bidirectional clipboard (UTF-8 text)"
