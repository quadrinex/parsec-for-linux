# Input backends: selection
- **uinput** (default, `PLH_INPUT_BACKEND=uinput`): generic, works wherever `/dev/uinput` is accessible (udev uaccess rule). Tested.
- **libei** (`ei.c`, `plh_ei_open_fd`): EIS fd from the RemoteDesktop portal. UNTESTED.
- Desktop geometry (`plh_detect_outputs`), automatic: KWin (`kscreen-doctor`, if KDE) > `wlr-randr` > `swaymsg -t get_outputs -r` (Wayland) > `xrandr --query` (X11). Parsers are tested on sample text (`make -C src/input test`); only KWin is verified on a real machine. Otherwise falls back to 5760x1080 with a warning.

## Desktop resize (virtual output)

`plh_uinput_resize()` recreates only the absolute pointer device when the KWin desktop rectangle changes. While a button is held it is deferred and applied on the last button release (`release_all` on client disconnect goes through the same path). During the deferral the old range stays in force, so after a desktop growth the pointer cannot reach the new output until the release (changing the range mid-drag would warp the pointer). Measured: creation 25-30 ms, KWin discovery 76-83 ms.
