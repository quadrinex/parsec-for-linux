# API map: what the official Parsec Windows app uses under Wine (v1, 2026-10-30)

Owner: eclaireur. Parsec 150-104a (and 150-105c, which it downloads itself), run by `parsecd.exe` (per-user install) in `runtime/prefix`. Wine is `wine-plh` (wine-staging 11.20 + `runtime/patches/wine/`). DXVK 3.1.1.
Observation is external only: import tables (`winedump -j import`), `WINEDEBUG` (+loaddll, +seh, +wtsapi, targeted +relay), winedbg backtraces, and the Parsec log `%APPDATA%\Parsec\log.txt` (= `runtime/prefix/drive_c/users/$USER/AppData/Roaming/Parsec/log.txt`).

Confidence levels:
- **P** = proven by a trace or log line.
- **S** = static only: the import is present, but the call has not been seen.
- **H** = hypothesis.

## How to reproduce
```sh
runtime/setup-prefix.sh                         # prefix + wine-plh + DXVK
runtime/run-parsec.sh --trace +loaddll,+seh     # log: runtime/traces/parsec-latest.log
LOG=runtime/prefix/drive_c/users/$USER/AppData/Roaming/Parsec/log.txt
winedump -j import "runtime/prefix/drive_c/users/$USER/AppData/Roaming/Parsec/parsecd-150-104a.dll"
```

## Summary by area

| Area | API / DLL observed | Conf. | Evidence | Consequence for the team |
|---|---|---|---|---|
| **Screen capture** | **DXGI Desktop Duplication** (`IDXGIOutput1::DuplicateOutput` or `IDXGIOutput5::DuplicateOutput1`). It returns `DXGI_ERROR_UNSUPPORTED` under DXVK. | **P** | `$LOG` while a client was connected (2026-10-30 08:02:38): `DUPLICATE: DXGI_ERROR_UNSUPPORTED`, repeated about once per second. Imports: `dxgi.CreateDXGIFactory1/2`, `d3d11.D3D11CreateDevice`. | **C4**: implement DuplicateOutput (proxy dxgi). Which variant (Output1 or Output5 `DuplicateOutput1`) is not traced yet: implement both. |
| Screens | `EnumDisplayMonitors`, `GetMonitorInfoW`, `EnumDisplayDevicesW`, `EnumDisplaySettingsW`, `QueryDisplayConfig`, `DisplayConfigGetDeviceInfo`, `SetDisplayConfig`, `MonitorFromPoint/Window`, `GetDpiForMonitor` | S | import table of parsecd-150-104a.dll | KWin positions must match these APIs (winex11 on XWayland today). |
| D3D | dxgi.dll and d3d11.dll (native, DXVK), d3d12.dll, d3d9.dll, winevulkan, opengl32 loaded at startup | P | `grep Loaded runtime/traces/parsec-latest.log` | |
| **Encoder** | **Media Foundation** loaded at startup: `mf.dll`, `mfplat.dll`, `mfreadwrite.dll`, `rtworkq.dll`. **No attempt to load `amfrt64.dll` or `nvEncodeAPI64.dll` seen** (the vendor DLLs are absent from the prefix, and failed loads are not yet traced). | P (MF loaded) / H (what it is used for) | +loaddll at startup. `vp_supported = 0` in `$LOG` on connection. | **N3**: next trace = `+relay` on `LoadLibraryExW/GetProcAddress` + `+mfplat` during a connection, to see whether Parsec looks for amfrt64 (AMF) or enumerates MFTs (`MFTEnumEx`). Not decided yet. |
| **Mouse input** | Parsec first tries its virtual mouse through **PCVUD (Parsec Virtual USB Adapter Driver)**, which fails, then (H) falls back to `SendInput`/`SetCursorPos`. Imports: `SendInput`, `SetCursorPos`, `GetCursorInfo`, `ClipCursor`, `SetCursor`, `GetRawInputData`, `RegisterRawInputDevices`, `SetWindowsHookExW`, `OpenInputDesktop`/`SetThreadDesktop`. | P (vmouse failure) / S (SendInput) | `$LOG`: `vmouse_create/PCVUD_vmouse_device_create[51] = -536870911`, `PCVUD_is_operable failed, no compatible driver found` | **I3**: intercept SendInput + SetCursorPos (to be confirmed with relay during a session). |
| **Gamepads** | **ViGEm** (fails) and PCVUD (fails). `XINPUT1_4.dll` and `hid.dll` are loaded. `SwDeviceCreate`, `SetupDi*`, `DiInstallDevice` are imported (driver install). | P | `$LOG`: `Failed to initialize host gamepad input via ViGEm`, `Parsec Virtual USB Adapter failed to initialize` | **I4**: no Windows driver possible. Look for a public path (ViGEm client API via its device interface?) at a later stage. |
| Audio | `mmdevapi.dll` loaded (WASAPI). Capture not yet traced during a session. | P (loaded) / H (loopback) | +loaddll. critique: `tests/t40-audio-loopback.sh` passes (WASAPI loopback works in this prefix). | E4: probably nothing to code. |
| Clipboard | `OpenClipboard`, `GetClipboardData`, `SetClipboardData`, `EmptyClipboard`, `AddClipboardFormatListener`, `GetClipboardSequenceNumber`, `IsClipboardFormatAvailable` | S | imports | E5: Windows→KDE works, KDE→Windows reads empty (critique, t41) → XWayland focus issue. |
| Network | WinHTTP with WebSocket (`WinHttpWebSocket*`) to `kessel-ws-v2.parsec.app`. STUN, UDP via ws2_32 (`WSASendMsg`, `WSAPoll`). TLS via SChannel/secur32. bcrypt AES-GCM. | P | `$LOG`: `Using websocket host 'kessel-ws-v2.parsec.app'`, `STUN reply`, `BUD AES_GCM = 256`, peer connected. Many `ws_async: WebSocket error: dwError:0x2F78` (12152 = ERROR_WINHTTP_INVALID_SERVER_RESPONSE) | It works (the peer connected), but the WinHTTP WebSocket error loop should be investigated (log noise, CPU). |
| Session | `WTSEnumerateSessionsW`, `WTSQuerySessionInformationW(WTSSessionInfoEx=25)`, `WTSQueryUserToken`, `ProcessIdToSessionId` | P | `+wtsapi`: class 25 is polled in a loop; **no window until it is implemented** | Fixed in wine-plh (patch 0002). |
| Process | parsecd.exe (loader, `LOADER_V17`) → child process `parsecd.exe LOADER_V17 PARSEC_IPC_<id>` (named-pipe IPC) | P | `pgrep -af parsecd` | |

## Wine fixes needed to start (wine-plh, `runtime/patches/wine/`)
1. **0001 ntdll `LdrLoadDll`**: startup crash without it.
   - Backtrace (winedbg): winhttp → crypt32 (delay-load) → `LdrResolveDelayLoadedAPI` → **a hook installed by parsecd-150-104a.dll on LdrLoadDll** → `LdrLoadDll(search_path=0x800, …)` → `wcslen(0x800)` → page fault.
   - 0x800 is `LOAD_LIBRARY_SEARCH_SYSTEM32`, passed as a flag value and not as a pointer.
   - Fix: a `search_path < 0x10000` is treated as flags; such a value can never be a valid pointer on Windows.
   - Test without Parsec: `wine build/runtime/ldrpath-test.exe 0x800` (source `runtime/tools/ldrpath-test.c`) crashes on unpatched wine-staging (`PLH_WINE_FLAVOR=staging`) and prints `OK` on wine-plh.
   - The failure is timing-dependent: with +relay, Parsec sometimes gets through.
2. **0002 wtsapi32 `WTSSessionInfoEx`**: without it, Parsec polls in a loop and never shows its window. It now returns an active, unlocked console session.
   - Proof: after the patch, the `MTY_Window "Parsec"` window appears (`wine build/runtime/winctl.exe list`).

## Account
The UI shows the Parsec login screen. The log shows a session (`sharedserv-…` connected at 08:02), so the account was opened by the human outside eclaireur.

## Next traces (v2)
- During a client connection: relay on `kernel32.LoadLibraryExW/LoadLibraryW/GetProcAddress`, `user32.SendInput/SetCursorPos`, `dxgi`, +mfplat, `mmdevapi` loopback.
- DuplicateOutput variant (Output1 or Output5).
- Cause of the `WebSocket 0x2F78` errors.

## winhttp WebSocket: `ws_async ... 0x2F78` loop (2026-10-30)
- Proven cause: Parsec (libmatoya) uses async `WinHttpWebSocket*` against `kessel-ws-v2.parsec.app`. The connection was established at 08:01; at 11:44 the peer dropped the TCP connection without a close frame, and from then on Wine's `receive_bytes` returned `ERROR_WINHTTP_INVALID_SERVER_RESPONSE` (0x2F78) on **every** following Receive (28k+ lines). Reproduced without Parsec: `runtime/tools/winhttp-ws/run.sh` (plain Wine: 0x2F78 ~100k times in 4 s).
- Fix: `runtime/patches/wine/0003` (winhttp in `PLH_WINE_MODULES`) -> `ERROR_WINHTTP_CONNECTION_ERROR` (0x2EFE, a Windows code). Proven by the same test. **Not proven**: that Parsec then reconnects (needs a real drop; otherwise the Receive loop would be in the app).
- Account: the session is persisted (user.bin + a `session_id` sent to the WS; the log shows the account's initial username after a parsecd restart). A return to the login screen was not observed in the logs.

## api-map v2: trace of a real client session (2026-10-30 13:27, `runtime/traces/parsec-20261030-132543.log`)
- **Capture**: `IDXGIOutput5::DuplicateOutput1` (formats 87 = B8G8R8A8, 28 = R8G8B8A8); logged by the `plh-dxgi` proxy: DP-2 stream 1 then HDMI-A-2 stream 2, 1920x1080, return 0 (`grep DuplicateOutput1 <trace>`).
- **Encoder order** (order of the trace lines): `nvcuda.dll`+`nvcuvid.dll` (l.5982, NVDEC/CUDA) -> `amfrt64.dll` (l.5986, then reloaded at l.29675, 32509, 35264, 119668) -> `nvEncodeAPI64.dll` (l.29673) -> `mfplat.MFTEnumEx` (l.36314, 126170), which loads `mfh264enc.dll`: Wine's software MFT encoder, the one retained (Parsec log: `encode_init = -15101/-15007/-10` for the failures, then `encode_x=1920 encode_y=1080`, FPS 59.4-60 at 13:27:49-13:28:03, latency ~8 ms). `CODECAPI_AVEncMPVGOPSize` and `AVEncNumWorkerThreads`: 0x80004001 (not implemented). `MFCreateDXGIDeviceManager`: not called.
- **Input**: `user32.SendInput` called 120 times (INPUT size 0x28, 1 event per call), a single thread 0x278 then 0x344. `ClipCursor(NULL)` only at startup (3 calls); no `SetCursorPos` seen.
- **Audio (E4)**: `mmdevapi.dll` loaded (builtin); live proof during the session: `pactl list source-outputs` -> Source Output #8010, `application.name=Parsec`, binary `wine-preloader`, `stream.capture.sink = "true"` (capture of an output's monitor, source 62). So WASAPI loopback -> PipeWire works without code. Non-silent content still has to be proven (t40 by critique).

## Network outside the LAN: -6023 (2026-10-30)
- Observed (Parsec log): `failed to connect (-6023)` three times within about a minute for a client outside the LAN; `STUN reply` only from an AWS IPv6 address; LAN sessions OK shortly before and after (peers `net = BUD|<global IPv6 of the LAN>`).
- Refuted in Wine (PE `ds.exe`, dual-stack socket, `IPV6_V6ONLY=0`): sending to `::ffff:127.0.0.1` works, the reply is received with source `::ffff:127.0.0.1`; `getaddrinfo("stun.parsec.app")` returns 3 IPv4 addresses. So Winsock is not at fault on those two points.
- Settled by passive capture: `runtime/tools/udpsniff/udpsniff.py` (sudo, AF_PACKET, no payload) traces the UDP flows of port 22430 and the STUN packets; output in /tmp/plh-eclaireur/udp.log.
- Sniffer (external attempt): the external client sends ~1 STUN request/s (104 bytes) over IPv4, client public IPv4 -> host LAN address, port 22430; the IPv4 STUN replies from the STUN server arrive too; the host answers no incoming IPv4 packet and stops sending to the client's IPv4 after the first packet; it sends every second over IPv6 (0 incoming). The Parsec log never shows an IPv4 STUN reply. So every IPv4 packet received seems to be lost on the read side.
- Hypothesis tested (`runtime/tools/winsock-dualstack/rm.c`, WSARecvMsg on a dual-stack socket): with IPV6_PKTINFO **and** IP_PKTINFO enabled, an IPv4 packet produces TWO cmsgs (level 41 type 19 + level 0 type 19, 64 bytes); with a 40-byte control buffer Wine fails with `WSAEMSGSIZE` (10040), flags=MSG_CTRUNC (0x200), and the datagram is lost. An IPv6 packet has one cmsg and gets through.
- The `+winsock` trace of a real attempt shows that Parsec enables `IPV6_V6ONLY=0`, `IPV6_PKTINFO` and `IP_PKTINFO` on the UDP socket of port 22430 and receives with `WSARecvMsg`; its control buffer size is not in the trace.
- Fix: patch `runtime/patches/wine/0004` (ntdll `convert_control_headers` drops `IP_PKTINFO` when `IPV6_PKTINFO` is present, as Windows reports only the IPv6 one). Proven with `rm.c` (40-byte control buffer: lost datagram on wine-staging, received on wine-plh). **Not proven end to end** with Parsec and an external client.

## Virtual display spike (S1 + S5 of docs/plan-virtual-display.md, 2026-11-08)

Observation only. **PROVEN** = read from a trace/log/source in this repository or the Wine 11.20 source; **INFERRED** = deduced, not shown by a test; **UNKNOWN** = needs the human's connection.

### S1 — what existing traces already say
- **PROVEN** (`runtime/traces/parsec-20261030-132543.log`, a real session, RelayInclude limited to `LoadLibrary*;GetProcAddress;MFTEnumEx;SendInput;SetCursorPos;ClipCursor`): Parsec resolves `CreateDXGIFactory1` by `LoadLibraryExW(L"dxgi.dll")` + `GetProcAddress` (about 1290 loads, 2999 GetProcAddress of that name across `dxgi.dll` = our proxy and `dxgi_dxvk.dll`), in several `parsecd.exe` processes. During one connected period (lines 28921 to 118112, 13:27:44 to 13:28:03) about 860 factories are created, so roughly 45 per second: Parsec does **not** enumerate once, it recreates the DXGI factory continuously while connected (**INFERRED** purpose: hotplug/output-change polling or a capture retry loop; both would make it notice a new output without any Windows message).
- **PROVEN** (same trace): `cfgmgr32.dll` loaded 117 times with `GetProcAddress` of `CM_Locate_DevNodeW`, `CM_Open_DevNode_Key`, `CM_Get_Device_ID_ListW`, `CM_Get_Device_ID_List_SizeW` (device-tree queries; which feature they serve is **UNKNOWN**).
- **PROVEN** (same trace): `user32.SendInput` x150 (INPUT size 0x28) and `ClipCursor(NULL)` x5; no `SetCursorPos`. Input is aimed with SendInput absolute coordinates, not SetCursorPos.
- **PROVEN** (import table, `runtime/traces/static/dll104a.imports.txt`, produced earlier): the Parsec DLL imports `EnumDisplaySettingsW`, `EnumDisplayDevicesW`, `EnumDisplayMonitors`, `GetMonitorInfoW`, `MonitorFromPoint/Window`, `QueryDisplayConfig`, `DisplayConfigGetDeviceInfo`, `SetDisplayConfig`, `GetSystemMetrics`, `GetDpiForMonitor`, `CreateDXGIFactory1/2`, and a SetupAPI/CM_* set. **Imported is not called**: no trace shows which of them run.
- **PROVEN** (`runtime/traces/parsec-20261030-073801.log`, full +relay of a host start): the `EnumDisplayDevicesW` / `EnumDisplaySettingsExW` / `ChangeDisplaySettingsExW(CDS_UPDATEREGISTRY|CDS_GLOBAL|CDS_NORESET)` / `GetSystemMetrics(SM_*VIRTUALSCREEN)` calls at lines 607671-607782 come from Wine's own builtin `explorer.exe` (loaded at line 396693), not from Parsec. Do not attribute them to Parsec.
- **PROVEN** (Parsec log, every connection, e.g. `runtime/traces/parsec-20261103-124500.log` lines 1067-1070): order `PCVUD_is_operable failed` -> `Using ViGEm` -> `'QueryDisplayConfig' failed with error 0x1F` -> `Failed to open MonitorDataStore key: 2` -> `'MM_Connect' failed with error -1` -> `Failed to initialize virtual displays`. 0x1F is ERROR_GEN_FAILURE; 2 is ERROR_FILE_NOT_FOUND (registry key absent). The calls before `MM_Connect` (device path opened, if any) are **UNKNOWN** until the trace below.
- **PROVEN**: with the new trace on and no client, 20 s after the host start the only relayed display-related calls were `SetThreadDpiAwarenessContext` x4. Parsec does no display/monitor enumeration through user32 while idle; enumeration is tied to a connection.

### S1 — what is still unknown (needs one real connection)
(a) whether the display list sent to the client comes from user32 (`EnumDisplayDevicesW`/`EnumDisplayMonitors`) or only from DXGI (`EnumOutputs`, not observable from outside: COM vtable calls are not relayed; the callers of user32 from `dxgi_dxvk.dll` will show DXVK's own path); (b) whether `ChangeDisplaySettingsExW` is called for "match client resolution"; (c) the calls before `MM_Connect`; (d) re-enumeration on display switch (a new burst of calls at the switch), and whether a `WM_DISPLAYCHANGE` is needed (not traced: messages are not relay calls; a re-enumeration burst after `Wine`'s `DisplayModeChanged` is the indirect evidence).
Command for the human, see `runtime/trace-display-list.sh` header: `runtime/trace-display-list.sh`.
How to read the result: callers are `ret=` addresses; map them with the `+loaddll` lines in the same file (`0000000140000000` = parsecd.exe, `parsecd-150-*.dll`, `dxgi_dxvk.dll`, `dxgi.dll`).

### S5 — can Wine pick up a new or removed XWayland output live?
- **PROVEN (source, wine-11.20 `dlls/winex11.drv/xrandr.c:1224-1252`)**: winex11 already selects `RRCrtcChangeNotify|RROutputChangeNotify|RRProviderChangeNotify` on the root window and handles them with `xrandr14_device_change_handler`, which invalidates the mode cache and calls `NtUserCallNoParam_DisplayModeChanged`; that reaches `display_mode_changed()` (`dlls/win32u/sysparams.c:4431`) -> `update_display_cache(TRUE)` (forced re-read of the monitors from the driver, `x11drv_UpdateDisplayDevices`) -> `WM_DISPLAYCHANGE` to the desktop window. So on paper Wine **does** follow hotplug without a restart; no winex11 patch is evidently needed for the mechanism itself.
- **UNKNOWN (not tested)**: whether it works here. The project's observation ("Wine only knows the layout seen at start", `layout-watch`) may come from (1) XWayland not emitting the RRNotify events for KWin outputs, (2) the half-screen offset bug (primary/ordering), or (3) the consumers (Parsec/DXVK adapters, uinput range), not from Wine. A test needs a real output change (S2's native tool creating a `stream_virtual_output`) with a small exe that prints `EnumDisplayMonitors` and waits for `WM_DISPLAYCHANGE`, plus `WINEDEBUG=+x11drv` (event handler names are printed). I did not run it: it changes the human's desktop.
- **Feasibility of a patch 0006**: `winex11.drv` has a PE part (`dllmain.c`) and `winex11.so`; `build-wine-plh.sh` could add `winex11.drv` to `PLH_WINE_MODULES` and `winex11` to `PLH_WINE_UNIX` like `ntdll`. Blockers: the script configures with `--without-x`, which must go (and `--without-xinerama` / `--without-opengl` etc. must be reviewed); X11 headers are installed here (`libx11`, `libxrandr`, `libxi`, `libxcursor`, `libxfixes`, `libxcomposite`, `libxrender`, `libxext`: **PROVEN** with `pacman -Q` and `/usr/include/X11/extensions/Xrandr.h`). Cost: a configure change that touches every module's `config.h` (full rebuild of the 4 modules, the stamp changes), plus build time of one more driver. A patch, if the test above shows events are lost, would be small (**estimate** 20-60 lines in `xrandr.c`, e.g. also select `RRScreenChangeNotifyMask` and handle `RRScreenChangeNotify` the same way, since XWayland sends it on every layout change); if the events arrive but the cache does not refresh, the fix would be in `win32u` instead (already rebuilt via the 0005 route for user32, not win32u: another module to add).
- **Verdict**: *feasible, size small to medium, necessity not established.* Do the live test first (zero Wine change); build 0006 only if it fails.
