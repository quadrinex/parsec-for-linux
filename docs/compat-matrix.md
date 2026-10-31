# Compatibility matrix

Maintained by **critique**. Rule: a cell is only filled by a test in `tests/`
actually run on the stated machine; everything else is **not tested**.
Legend (phase 1): ✅ PASS · ❌ FAIL · — not tested. The ⏳ cells below are historical: all components are delivered, see `make test-full` (14/14) and the Phase 2 section.

Reference machine (the only one tested): Arch, KDE Plasma 6.8.1 Wayland, AMD RX 6600 (radeonsi/RADV), 3 screens 1920x1080, wine-staging 11.20 + DXVK 3.1.1 (prefix `runtime/prefix`).

## Phase 1 — reference machine (2026-10-30)

| Component | Test | State | Observation |
|---|---|---|---|
| Guardrails (system Wine, ROLLBACK, chmod, patch) | t00 | ✅ | System Wine 0 modified files; packages and `/etc` recorded |
| Native bridge primitives (AF_UNIX, memfd, SCM_RIGHTS) | t01 | ✅ | — |
| PE chain mingw-w64 → Wine | t02 | ✅ | wine-staging 11.20 |
| PE↔native transport (reproduction of D-005) | t05 | ✅ | TCP 127.0.0.1: mean ≈ 55–60 µs, p99 ≈ 135–155 µs; /dev/shm via Z: + CreateFileMapping: 300 1080p frames intact in each direction, ≈ 1.5 ms/frame (check included); **AF_UNIX winsock works under wine-staging** (socket + connect to a native socket by Z: path), refused (10047) by the system wine |
| Screen layout seen by Windows | t03 | ✅ | translation -1920,0: DISPLAY1=DP-2 (primary) 0,0; DISPLAY2=HDMI-A-2 1920,0; DISPLAY3=HDMI-A-1 -1920,0 |
| Native test pattern + checker (spectacle reference) | t10 | ✅ | 3 screens found at the KWin positions |
| DXGI Desktop Duplication capture (dxgi proxy) | t11 | ✅ | 3 outputs at the KWin positions, test pattern conforming, cursor; 1 stream ≈ 59 fps, 3 streams ≈ 29 fps (D-011) |
| Native VA-API H.264/HEVC encoding (N1 bench) | t20 | ✅ | 300 frames, IDR at 0/100/250, no B, CBR ±1 %, p99 ≈ 7–8 ms (1080p) |
| Encoding through the PE API (amfrt64.dll) | t21 | ✅ | AVC+HEVC, decoded BGRA content conforming, p50 latency ≈ 4 ms, absent bridge handled; NV12 not tested |
| SendInput/key/wheel input → desktop | t30 | ✅ | exact clicks on 3 screens (junctions included), F24, wheel; SetCursorPos: known limitation D-015 |
| Audio: WASAPI loopback of the default output | t40 | ✅ | without project code (winepulse); peak 0.018 for a 5 % sine |
| Windows ↔ KDE clipboard (UTF-8 text) | t41 | ✅ | src/clipboard gateway (plh-clipd + plh-clipsync), both directions |
| Real Parsec end to end | — | user validation | outside `tests/`: the user confirmed desktop, mouse, keyboard, sound from the outside; not automatable |

The intrusive tests (t10, t11, t12, t30, t40, t41) only run with `PLH_TEST_INTRUSIVE=1`.

## Phase 2 — other environments (update of the final review, 2026-10-30)

Three statuses, no others: **tested here** (test run on the reference machine, see the evidence column) · **not tested** (code present or not, never run in this environment: do not infer that it works) · **not supported** (not implemented, will not work).

| Environment / component | Status | Evidence or reason |
|---|---|---|
| KDE Plasma 6 / Wayland / AMD RX 6600: direct KWin capture | **tested here** | t11, t12 (1 stream ≈ 59–61 fps; 3 streams ≈ 29 fps, D-011); real session validated by the user |
| KDE / Wayland / AMD: VA-API H.264/HEVC encoding, PE AMF (amfrt64.dll) | **tested here** | t20, t21; real session |
| KDE / Wayland: input (SendInput → hook → uinput), 3 screens | **tested here** | t30 (clicks, F24, wheel); `SetCursorPos`: known limitation D-015 |
| KDE / Wayland: audio loopback, two-way UTF-8 text clipboard | **tested here** | t40, t41 |
| Automatic backend detection (Wayland/KDE/X11) | **tested here** (KDE path only) | the KDE path is the one of t11/t12; the other branches of the detection are not run on a matching real desktop |
| X11: XShm capture | **tested here under Xvfb only** | no real X11 desktop content verified; real KDE/X11: not tested |
| ScreenCast portal capture (GNOME, wlroots, KDE fallback) | **not tested** | compiled, never run; restore token never exercised |
| GNOME (Wayland or X11) | **not tested** | no GNOME session available |
| wlroots (Sway, Hyprland) | **not tested** | `wlr-randr`/`swaymsg` detection verified on sample outputs only; wlroots keyboard/mouse (wlr-virtual-pointer) not done |
| Input through libei (RemoteDesktop portal) | **not tested** | `ei.c` compiled, outside the build; XTest not done |
| Gamepads (uinput) | **not tested / not done** | absent |
| x264 software encoding (no GPU) | **tested here** (CPU inputs, H.264) | `make -C src/encode test`; software HEVC and the DMA-BUF→x264 path not covered |
| Intel GPU (VA-API iHD) | **not tested** | no Intel machine |
| NVIDIA GPU (NVENC) | **not supported** | `nvEncodeAPI64.dll` not implemented, NVENC session not implemented (probe only, not tested): NVIDIA hardware host not supported |
| Vulkan Video | **not supported** | absent |
| 3 screens 1920x1080, primary ≠ leftmost (reference) | **tested here** | t03, t11, t12, t30 |
| 1 screen, 2 screens | **not tested** | — |
| Screens of different sizes/scales | **not tested** | — |
| Connection outside the LAN | **not tested in isolation** | user validation from the outside, patch 0004; exact effect not isolated |
| 3 simultaneous streams at 60 fps | **not supported** | ≈ 29 fps (D-011, SHM/KWin limit) |
