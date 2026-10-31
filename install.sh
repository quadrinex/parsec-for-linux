#!/usr/bin/env bash
# Installs the project's dependencies and system changes, then builds everything.
# Each change is recorded in ROLLBACK.md and undone by uninstall.sh. Idempotent.
# Usage: ./install.sh [--system-only] [--dry-run]
# Tested on Arch Linux only; apt and dnf are untested (see docs/compat-matrix.md).
set -u
DRY=0; SYSTEM_ONLY=0
for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    --system-only) SYSTEM_ONLY=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done
HERE="$(cd "$(dirname "$0")" && pwd)"
run() { echo "+ $*"; [ "$DRY" = 1 ] || "$@"; }

if command -v pacman >/dev/null; then
  run sudo pacman -S --needed --noconfirm mingw-w64-gcc libva libei pipewire libxcb x264
elif command -v apt-get >/dev/null; then
  run sudo apt-get install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 libva-dev libei-dev libpipewire-0.3-dev libsystemd-dev libxcb1-dev libxcb-shm0-dev libxcb-randr0-dev libxcb-xfixes0-dev libx264-dev
elif command -v dnf >/dev/null; then
  run sudo dnf install -y mingw64-gcc mingw64-gcc-c++ libva-devel libei-devel pipewire-devel systemd-devel libxcb-devel x264-devel
else
  echo "unrecognized package manager: install the README dependencies yourself" >&2
fi

UDEV=70-parsec-linux-host-uinput.rules
MODS=70-parsec-linux-host-uinput.conf
run sudo install -m 644 "$HERE/src/input/udev/$UDEV" "/etc/udev/rules.d/$UDEV"
run sudo install -m 644 "$HERE/src/input/udev/$MODS" "/etc/modules-load.d/$MODS"
run sudo udevadm control --reload
run sudo udevadm trigger --name-match=uinput
run sudo modprobe uinput

[ "$SYSTEM_ONLY" = 1 ] && exit 0

run make -C "$HERE"
run "$HERE/runtime/setup-prefix.sh"
echo "installation complete. Next: bin/parsec-linux-host start, then sign in to Parsec."
