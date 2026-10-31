# Input backends: selection
- **uinput** (default, `PLH_INPUT_BACKEND=uinput`): generic, works wherever `/dev/uinput` is accessible (udev uaccess rule). Tested.
- **libei** (`ei.c`, `plh_ei_open_fd`): EIS fd from the RemoteDesktop portal. UNTESTED.
- Desktop geometry (`plh_detect_outputs`), automatic: KWin (`kscreen-doctor`, if KDE) > `wlr-randr` > `swaymsg -t get_outputs -r` (Wayland) > `xrandr --query` (X11). Parsers are tested on sample text (`make -C src/input test`); only KWin is verified on a real machine. Otherwise falls back to 5760x1080 with a warning.
