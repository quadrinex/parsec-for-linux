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
