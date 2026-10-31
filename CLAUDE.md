# parsec-for-linux — team source of truth

Read this file in full before acting. The lead keeps it up to date; if something in it is wrong, message the lead, do not change it yourself.

## Goal
Make this Linux PC a **Parsec host** by running the **official Windows Parsec app under Wine**, and implementing, as a compatibility layer, the public Windows APIs it uses (screen capture, hardware encoding, input, audio, clipboard) on top of their Linux equivalents. The client must see and control the **real KDE desktop**, not a VM.

## Machine (as of 2026-10-30)
- Arch Linux, kernel 7.2.11, root on **ext4** (no btrfs snapshots, so backups go to `backups/`).
- KDE Plasma 6.8.1 / KWin 6.8.1 on **Wayland** (`WAYLAND_DISPLAY=wayland-1`).
- Ryzen 5 5600, Radeon RX 6600 (Navi 23), Mesa 26.2.5 radeonsi. VA-API: H.264 (CB/Main/High) and HEVC Main encoding; no AV1 encoding.
- Screens (kscreen-doctor): HDMI-A-1 at 0,0; **DP-2 at 1920,0**; HDMI-A-2 at 3840,0; all 1920x1080. Desktop 5760x1080.
- PipeWire 1.6.9 (pipewire-pulse).
- Already present: wine 11.20 (system, **never modify it**), winetricks, yay, clang, cmake, gcc, ffprobe, vainfo, libei 1.6.0, xdg-desktop-portal-kde 6.8.1, lib32-* (pipewire, mesa, vulkan-radeon, libva).

## Guardrails (non-negotiable)
1. Do not format, repartition or mount for writing any other disk; do not touch Windows (dual boot, other disk), the bootloader or `/boot`.
2. Any system change (package, udev, module, file outside the repository and the dedicated Wine prefix): **first** a line in `ROLLBACK.md` (what, undo command), **then** the matching undo in `uninstall.sh`. The initial backup of /etc and package lists is in `backups/` (not versioned).
3. Never kill `kwin_wayland`, `plasmashell`, the session manager, or the session's PipeWire/WirePlumber. Do not restart the session.
4. No `chmod 777`, no security feature disabled. `/dev/uinput`: udev rule (`TAG+="uaccess"` or a dedicated group).
5. Do not modify, patch or disassemble Parsec binaries; do not bypass anything related to accounts or licensing. External observation only (WINEDEBUG, +relay, traces, Parsec logs) and implementation of **public Windows APIs**, as Wine does.
6. **Never** enter credentials. The human logs in to Parsec themselves.
7. The system Wine is never modified. Any DLL replacement goes through `WINEDLLOVERRIDES` / the registry of **the dedicated prefix only**.
8. Honesty: what is not proven by a test is not "done". Claims go in PROGRESS.md with the test command that proves them.

## Team and directory ownership
Each teammate edits **only** their directories. For a change elsewhere: message the owner.

| Teammate | Owns | Role |
|---|---|---|
| lead | `CLAUDE.md`, `PROGRESS.md`, `DECISIONS.md`, `ROLLBACK.md`, `README.md`, `NOTICE.md`, `LICENSE`, `Makefile`, `bin/`, `install.sh`, `uninstall.sh`, `packaging/` | integration, arbitration |
| eclaireur | `runtime/`, `docs/api-map.md`, `src/audio/`, `src/clipboard/` | Wine prefix, Parsec install, API tracing, audio, clipboard |
| capture | `src/capture/`, `src/bridge/` | real desktop capture (portal/PipeWire, KWin), DMA-BUF, bridge daemon |
| encodeur | `src/encode/` | the encoding API Parsec uses, on top of VA-API (then NVENC, software) |
| entrees | `src/input/` | keyboard/mouse/wheel on 3 screens, uinput gamepads |
| critique | `tests/`, `docs/compat-matrix.md` | test harness, measurements, adversarial review |

Shared protocol header: `src/bridge/proto/` (any change is announced to everyone).

## Architecture
**Thin PE DLLs + native Linux daemon `plh-bridge`** (see DECISIONS.md).

```
Parsec (PE, Wine, dedicated prefix runtime/prefix)
  ├─ dxgi.dll (PE proxy in front of DXVK dxgi): DuplicateOutput1, enumeration of the outputs
  ├─ amfrt64.dll (PE implementation of the public AMF API)
  ├─ plh-input-hook.exe: low-level hooks on the input Parsec injects with SendInput
  ├─ plh-clipsync.exe: text clipboard
  └─ WASAPI loopback: winepulse → PipeWire monitor
          │  TCP 127.0.0.1 + token (Wine 11.20 refuses AF_UNIX winsock, see D-005) + /dev/shm through Z:
          ▼
plh-bridge (native Linux, C): KWin screencast, portal, X11, PipeWire, VA-API, uinput
```

## Coordinates (critical, applies to capture and entrees)
Windows requires the primary screen at 0,0. KWin has **DP-2 as primary**, so under Wine: DISPLAY1 = DP-2 at 0,0; DISPLAY2 = HDMI-A-2 at 1920,0; DISPLAY3 = HDMI-A-1 at **-1920,0**. Windows space = KWin space − (1920,0). Never hard-code these values: derive the translation from the KWin layout (primary screen) at startup and on every configuration change.

## Input test safety (incident of 2026-10-30)
The human's session has KDE Connect "Share input devices" active: moving the pointer to an extreme edge of the desktop can switch all input to their phone. Rules for any test that injects:
- never within 8 px of an outer desktop edge (0, 5759, 0, 1079 in KWin space);
- probe first: one click in OUR test window, verified as received; otherwise abort at once with no further injection;
- check focus before each click or key; no key other than F24/Shift/Right Ctrl outside our test window;
- do not run an injection test while a Wine window (winedbg, Parsec dialog) may steal focus;
- never change the human's KDE Connect configuration;
- **one injector at a time**: any test that injects (native, uinput, libei or SendInput under Wine) holds the `flock /tmp/plh-input.lock` lock for its whole duration; margin raised to 50 px for critique's and entrees' tests.

## Conventions
- Language: C11 for native and PE (mingw, `-Wall -Wextra`), POSIX shell/bash for scripts. No heavy dependency without an entry in DECISIONS.md.
- All code, comments, messages and docs are in English.
- Build: each module has a local `Makefile` called by the root `Makefile` (`make`, `make test`). Build outputs in `build/` (not versioned).
- Small, frequent commits, prefixed by module: `capture: ...`, `encode: ...`. Commit only your files (`git add <your paths>`, never `git add -A`).
- Dedicated Wine prefix: `runtime/prefix` (not versioned), created by `runtime/setup-prefix.sh` (versioned, idempotent). Always `WINEPREFIX=$PWD/runtime/prefix`; never `~/.wine`.
- Large logs and traces: `runtime/traces/` (not versioned); summaries in the docs.
- Temporary files: `/tmp/plh-<name>/`.
- Every "it works" claim needs a test in `tests/`; critique writes or reviews them and actively tries to refute them.
- No personal data in the repository: no usernames, IP addresses, peer IDs or account details, including in logs quoted in docs.
