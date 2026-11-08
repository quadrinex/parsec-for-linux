# Log of system changes and how to undo them

Rule: one line **before** every change outside the repository and outside `runtime/prefix`. Each line has its undo in `uninstall.sh`.

Reference state: `backups/etc-20261030-072800.tar.gz`, `backups/pkglist-explicit-20261030-072800.txt`, `backups/pkglist-all-20261030-072800.txt`.

| # | Date | Author | Change | Undo |
|---|---|---|---|---|
| 0 | 2026-10-30 | lead | No system change at this point (backups only). | — |
| 1 | 2026-10-30 | eclaireur | `sudo pacman -S --needed mingw-w64-gcc` (pulls in mingw-w64-binutils, -crt, -headers, -winpthreads) | `pacman -Rns` of the mingw-w64-* packages absent from the initial list |
| 2 | 2026-10-30 | entrees | `/etc/udev/rules.d/70-parsec-linux-host-uinput.rules` (uinput, `TAG+="uaccess"`, `static_node=uinput`) + `udevadm control --reload && udevadm trigger --name-match=uinput` | `rm` the file + `udevadm control --reload` |
| 3 | 2026-10-30 | entrees | `/etc/modules-load.d/70-parsec-linux-host-uinput.conf` (`uinput`) + `modprobe uinput` | `rm` the file (module not unloaded: steam/kdeconnect use it too) |
| 4 | 2026-10-30 | entrees | `~/.config/kcminputrc`, group `[Libinput][7531][28674][PLH virtual mouse]` : `PointerAccelerationProfile=1` (flat), `PointerAcceleration=0` — our virtual mouse only, set through KWin DBus | `kwriteconfig6 --delete` of the two keys of this group |
| 5 | 2026-10-30 | capture | Project user data: `~/.config/parsec-linux-host/` (portal restore token, settings) and `$XDG_RUNTIME_DIR/plh-bridge/` (bridge token, socket; volatile) | `rm -rf` of both directories |
| 6 | 2026-10-30 | lead | Optional, only on `bin/parsec-linux-host enable` : `~/.config/systemd/user/parsec-linux-host.service` | `bin/parsec-linux-host disable` |
| 7 | 2026-10-31 | eclaireur | `bin/parsec-linux-host launcher add` (run by `install.sh` and `enable`): `~/.local/share/applications/parsec-for-linux.desktop` (menu entry "Parsec") and the icon `~/.local/share/icons/hicolor/*/apps/parsec-for-linux.{png,svg}`, produced from the local Parsec install or a generic icon | `bin/parsec-linux-host launcher remove` |
| 8 | 2026-10-31 | eclaireur | Optional, on `bin/parsec-linux-host enable`: `~/.config/systemd/user/parsec-tray.service` (tray icon at login); `~/.local/share/parsec-for-linux/config.env` when the settings window is saved from a read-only install | `bin/parsec-linux-host disable` (removes the unit); the data directory is removed by line 5's `rm -rf` of `~/.config/parsec-linux-host` only for the config; `uninstall.sh` also removes `~/.local/share/parsec-for-linux/config.env` |
| 9 | 2026-11-08 | capture | Virtual display spike S2 (`vout-probe`): KWin records a virtual output named `Virtual-plh-*` in `~/.config/kwinoutputconfig.json` (it reused the slot of an earlier virtual output and added one 4-output setup). Baseline copy: `backups/kwinoutputconfig.before-vout-spike.json` | `uninstall.sh` step [9]: puts the slot back from the baseline and drops the setups added since (without the baseline: drops the setups using that slot). Tested on a copy: result identical to the baseline, second run is a no-op |
