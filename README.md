# parsec-for-linux

Turns a Linux PC into a Parsec host: the official Windows Parsec app runs under Wine, and a compatibility layer connects its capture, encoding and input APIs to the real desktop.

**Status:** experimental. `PROGRESS.md` lists what has actually been proven. Tested on one machine only: Arch Linux, KDE Plasma 6.8 on Wayland, Radeon RX 6600, 3 1080p screens. For anything else, see `docs/compat-matrix.md`.

This project is not affiliated with Parsec. See `NOTICE.md`.

## How it works

| Function | What Parsec calls | What answers |
|---|---|---|
| Capture | DXGI Desktop Duplication | The project's `dxgi.dll`, in front of DXVK; KWin capture through `plh-bridge` |
| Encoding | AMF (`amfrt64.dll`) | The project's `amfrt64.dll`; VA-API through `plh-bridge` |
| Input | `SendInput` | `plh-input-hook.exe`; uinput through `plh-bridge` |
| Audio | WASAPI loopback | Wine, to the PipeWire monitor of the default output |
| Gamepads | ViGEm interface | The project's `plh-vigem.sys`, in the prefix under Wine; a uinput Xbox 360 pad through `plh-bridge` |
| Clipboard (text) | Windows API | `plh-clipsync.exe` and `plh-clipd.sh` |

Parsec's binaries are not modified. Neither is the system Wine: the project uses its own patched copy and a dedicated prefix.

## Installation

```sh
./install.sh --dry-run   # shows what would be done
./install.sh             # packages, udev rule for /dev/uinput, build, Wine, DXVK, prefix, Parsec
```

`install.sh` supports pacman, apt and dnf; only Arch has been tested. It uses sudo for system changes, each of which is listed in `ROLLBACK.md`.

Then open Parsec and log in to your account. The project never handles your credentials.

An Arch package definition is in `packaging/arch/PKGBUILD`.

## Usage

```sh
bin/parsec-linux-host open      # starts the host if needed and shows the Parsec window (also the "Parsec" menu entry)
bin/parsec-linux-host config    # settings window, in the tray program (needs PySide6; the host runs without it)
bin/parsec-linux-host launcher add   # menu entry and icon (made from your own Parsec install); `launcher remove` undoes it
bin/parsec-linux-host start     # starts everything
bin/parsec-linux-host status    # component and client state
bin/parsec-linux-host stop      # stops everything
bin/parsec-linux-host enable    # starts with the session (systemd user services: host and tray) and adds the menu entry
bin/parsec-linux-host disable   # reverts enable
```

`stop` ends a connected client's session. A connected client controls your real desktop.

## Tests

```sh
make test        # no visible effect on the desktop
make test-full   # shows test patterns, injects input and plays a sound; fails if any test is skipped
```

## Measurements

From the tested machine, 1080p:

| | |
|---|---|
| Parsec session | 58.8 to 60 FPS, 13 to 15 ms latency as reported by Parsec |
| Encoding alone | 3.16 ms median (native benchmark) |
| Capturing 1 screen | about 60 FPS |
| Capturing 3 screens at once | about 29 FPS |

## Known limitations

- Hardware encoding through the Parsec path uses AMF, then VA-API. NVIDIA and Vulkan Video are not available.
- Capturing all 3 screens at once caps at about 30 FPS; one screen holds about 60.
- H.264 is limited to 4096 px wide on the tested GPU.
- Gamepads: the mechanism is proven with a ViGEmClient-like probe, not yet with a real Parsec client. Rumble is not sent back to the client.
- `SetCursorPos` does not reach the real desktop; Parsec only used `SendInput` in the observed sessions.
- The cursor did not appear on a phone client; other clients are untested.
- Clipboard is text only and has not been tried with a real client.
- Portal and X11 capture are not validated on real desktops other than KDE.
- Parsec's service mode, the login screen and the lock screen are not supported.

## Troubleshooting

| Symptom | What to check |
|---|---|
| Green or black screen on the client | `bin/parsec-linux-host status`, then check that `amfrt64.dll` and the `dxgi.dll` proxy are in the prefix's `drive_c/windows/system32/` |
| Clicks and keys have no effect on the desktop | Disconnect the client; check `plh-input-hook` in `status` and `plh-input-hook.log` in the traces directory |
| Local clicks and keys are blocked while the cursor moves | Run `bin/parsec-linux-host stop`: a Wine window may have kept the grab |
| With KDE Connect "Share input devices", input goes to the phone | Bring the pointer back from the phone, or disable that plugin |
| "Parsec is either out of date or corrupt" (File System Error - Skel Crash), or `status` shows `parsecd blocked` | Parsec was stopped while starting 3 times in a row and its crash counter refuses to load it. Its files are fine. Set `"count"` back to 0 in `cc*.txt` next to Parsec's `log.txt` (or reinstall Parsec), then `bin/parsec-linux-host restart`. `stop` now waits until Parsec has finished starting |
| Mouse on the wrong screen, two identical screens in Parsec's list | `bin/parsec-linux-host restart`: Wine kept an incomplete screen layout |
| `-6023` when connecting from outside the LAN | Try on the LAN first to isolate the problem. Check that you use the patched `wine-plh` build (`runtime/build-wine-plh.sh`) |

Logs: `runtime/traces/` and `AppData/Roaming/Parsec/log.txt` in the prefix. The prefix is `runtime/prefix`, or `~/.local/share/parsec-for-linux` when the repository is not writable.

## Uninstalling

```sh
./uninstall.sh --dry-run   # shows what would be done
./uninstall.sh             # reverts system changes and deletes the prefix
```

Each system change is described in `ROLLBACK.md`.

## License

MIT, except the parts listed in `NOTICE.md`.
