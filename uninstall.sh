#!/usr/bin/env bash
# Undoes all system changes recorded in ROLLBACK.md.
# Each block matches a ROLLBACK.md line (same number). Idempotent.
# Usage: ./uninstall.sh [--dry-run] [--keep-prefix]
set -u
DRY=0; KEEP_PREFIX=0
for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    --keep-prefix) KEEP_PREFIX=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done
HERE="$(cd "$(dirname "$0")" && pwd)"
run() { echo "+ $*"; [ "$DRY" = 1 ] || "$@"; }

# Stop the host if it is running.
if [ -x "$HERE/bin/parsec-linux-host" ]; then run "$HERE/bin/parsec-linux-host" stop || true; fi

# The tray icon (it survives host restarts, so `stop` leaves it).
run pkill -f 'bin/parsec-tra[y]' || true

# --- Undo steps (one per ROLLBACK.md line) ---
BASE_PKGS="$HERE/backups/pkglist-all-20261030-072800.txt"
was_installed() { [ -f "$BASE_PKGS" ] && grep -q "^$1 " "$BASE_PKGS"; }

# [8] tray unit, settings saved outside the repository  ([6] and [8] units are removed by `disable`)
run rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/parsec-for-linux/config.env"
# [7] application menu entry and icon
if [ -x "$HERE/bin/parsec-linux-host" ]; then run "$HERE/bin/parsec-linux-host" launcher remove; fi
# [6] optional user systemd service (host and tray)
if [ -e "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/parsec-linux-host.service" ] || [ -e "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/parsec-tray.service" ]; then
  run "$HERE/bin/parsec-linux-host" disable
fi
# [5] project user data
run rm -rf "${XDG_CONFIG_HOME:-$HOME/.config}/parsec-linux-host" "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/plh-bridge"
# [4] flat acceleration profile of the virtual mouse (kcminputrc)
if command -v kwriteconfig6 >/dev/null; then
  for k in PointerAccelerationProfile PointerAcceleration; do
    run kwriteconfig6 --file kcminputrc --group Libinput --group 7531 --group 28674 --group "PLH virtual mouse" --key "$k" --delete
  done
fi
# [3] uinput module at boot
run sudo rm -f /etc/modules-load.d/70-parsec-linux-host-uinput.conf
# [2] uinput udev rule
if [ -e /etc/udev/rules.d/70-parsec-linux-host-uinput.rules ]; then
  run sudo rm -f /etc/udev/rules.d/70-parsec-linux-host-uinput.rules
  run sudo udevadm control --reload
fi
# [1] mingw-w64 (only the packages absent from the initial state)
rm_pkgs=()
for p in mingw-w64-gcc mingw-w64-binutils mingw-w64-crt mingw-w64-headers mingw-w64-winpthreads; do
  pacman -Q "$p" >/dev/null 2>&1 && ! was_installed "$p" && rm_pkgs+=("$p")
done
[ ${#rm_pkgs[@]} -gt 0 ] && run sudo pacman -Rns --noconfirm "${rm_pkgs[@]}"

# --- Project-specific data ---
. "$HERE/runtime/env.sh"
if [ "$KEEP_PREFIX" = 0 ] && [ -n "$WINEPREFIX" ] && [ -d "$WINEPREFIX" ]; then
  run rm -rf "$WINEPREFIX"
fi
echo "uninstall complete."
