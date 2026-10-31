# plh-bridge — PE (Wine) <-> native bridge

Owner: capture. Protocol: `proto/plh_proto.h` (+ `proto/plh_proto_<service>.h`).

## Transport (measured on 2026-10-30, system Wine 11.20)
| Path | Result | Proof |
|---|---|---|
| AF_UNIX winsock from an .exe | **refused**: `WSAEAFNOSUPPORT` (10047) | `wine build/bridge/afunix-probe.exe` |
| TCP 127.0.0.1 PE -> native | OK, RTT ~50-100 µs | `plh-captest.exe` (`ping` line) |
| `/dev/shm` via `Z:\dev\shm` + CreateFileMapping | OK, coherent both ways | `run-bridge-tests.sh` test 6 |

So: PE -> TCP 127.0.0.1 (ephemeral port) + 32-byte token; native -> AF_UNIX (SO_PEERCRED, SCM_RIGHTS fds).
`$XDG_RUNTIME_DIR/plh-bridge/` (0700) contains `bridge.sock`, `port`, `token` (0600).
Under Wine, `XDG_RUNTIME_DIR` is renamed `WINE_HOST_XDG_RUNTIME_DIR`: the client library handles it;
`PLH_BRIDGE_DIR` forces the directory.

## Using the bridge from a module
- **PE side or native tool**: compile `client/plh_client.c` with your DLL (`-lws2_32` for PE). `plh_conn_open()`,
  `plh_conn_request()`, `plh_conn_send()`, event callback, `plh_shm_map()`.
- **Daemon side**: create `src/<module>/service.mk` (adds to `SERVICE_SRCS`, `SERVICE_PKGS`, `SERVICE_LIBS`,
  `SERVICE_CFLAGS`, `SERVICE_WL_PROTOCOLS`, paths relative to the root). In the code:
  `static const struct plh_service svc = {.name, .id = PLH_SVC_xxx, .init, .fini, .handle, .client_gone};
  PLH_SERVICE_REGISTER(svc)`. API: `plh_bridge.h` (single-threaded epoll loop, timers, `plh_reply`,
  `plh_send_event`, `plh_shm_create`). Messages: define `proto/plh_proto_<service>.h` in your range
  (0x02 encoding, 0x03 input, 0x04 audio, 0x05 clipboard) and send the file to capture, who adds it.

## Capture service (0x01) — status
- `CAP_LIST_OUTPUTS`: real outputs via Wayland (wl_output v4 + xdg_output) or RandR on X11, sorted by position.
- `CAP_OPEN` backends: `test` (60 fps test pattern, frame counter on row 0, synthetic cursor);
  `kwin` (zkde_screencast, no dialog, tested); `x11` (XShm, tested under Xvfb);
  `portal` (ScreenCast + PipeWire, not tested).
- Frame: `CAP_FRAME` event (slot held until `CAP_RELEASE`), global `frame_id` for the encoder.

## Tests
`make -C src/bridge test` -> `tests/run-bridge-tests.sh` (TAP): native ABI = PE, 0600 token, native unix client,
native TCP client, wrong token refused, PE client under Wine (throwaway prefix `/tmp/plh-capture/wpfx-test`),
portal restore-token file (0600, symlink refused).
Manual tool: `build/bridge/plh-captest [-b test|portal|kwin|x11|auto] [-n N] [-o dir]` -> `<output>.png`.
