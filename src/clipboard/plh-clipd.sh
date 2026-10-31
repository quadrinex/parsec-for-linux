#!/usr/bin/env bash
# plh-clipd : Linux half of the clipboard gateway (Klipper D-Bus <-> $XDG_RUNTIME_DIR/plh-bridge/clip, 0700).
# Files are deleted as soon as they are consumed. Usage: plh-clipd.sh [seconds]. Text only.
umask 077
D="$XDG_RUNTIME_DIR/plh-bridge/clip"; mkdir -p "$D"; chmod 700 "$XDG_RUNTIME_DIR/plh-bridge" "$D"
K="qdbus6 org.kde.klipper /klipper"; trap 'rm -f "$D"/*' EXIT
end=0; [ -n "${1:-}" ] && end=$(( $(date +%s) + $1 ))
last=$($K getClipboardContents 2>/dev/null || true)
while [ "$end" = 0 ] || [ "$(date +%s)" -lt "$end" ]; do
    cur=$($K getClipboardContents 2>/dev/null || true)
    if [ "$cur" != "$last" ]; then last=$cur; printf '%s' "$cur" > "$D/linux.tmp" && mv -f "$D/linux.tmp" "$D/linux.txt"; fi
    if [ -f "$D/windows.txt" ]; then
        w=$(cat "$D/windows.txt"); rm -f "$D/windows.txt"
        if [ "$w" != "$last" ]; then last=$w; $K setClipboardContents "$w" >/dev/null 2>&1; fi
    fi
    sleep 0.3
done
