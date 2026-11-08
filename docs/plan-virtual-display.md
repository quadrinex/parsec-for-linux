# Plan: virtual display (not built)

Status: **plan only**. Nothing below is implemented or proven. Every step ends with the test that proves it.

## What the user wants

- **F1. On demand.** The Parsec client's display list offers a "Virtual display" entry. The virtual screen exists **only while it is selected**: selecting it creates it, selecting another display or disconnecting removes it.
- **F2. Screens off.** If a client connects while the real screens are off, the session goes to the virtual display automatically, and it is removed on disconnect. The real screens stay off.

## What we already know

- Parsec has its own virtual display feature (`Program Files/Parsec/vdd/parsec-vdd.exe`, an IddCx driver). It tries it **at every client connection** and fails; the log shows, in order: `'QueryDisplayConfig' failed with error 0x1F`, `Failed to open MonitorDataStore key: 2`, `'MM_Connect' failed with error -1`, `Failed to initialize virtual displays`.
- KWin 6.8.1 offers `zkde_screencast_unstable_v1.stream_virtual_output[_with_description]` (version ≥ 2/4, ours is 6): it **creates a real virtual output and feeds it as a stream**; the output belongs to the stream. That is exactly the F1 lifetime, and it is crash-safe: if the bridge dies, KWin drops the output (to be proven, S2).
- Our `dxgi.dll` proxy does not wrap `EnumOutputs` today: DXVK lists Wine's monitors, and `match_output()` maps each to a bridge output by position.
- Wine only knows the screen layout seen at start (the reason for `layout-watch`), and so does the uinput absolute pointer device (its range is the desktop size at start).

## Two ways, one chosen

**A. Make Parsec's own virtual display work.** Its option would appear in Parsec as on Windows. But `MM_Connect` talks to Parsec's private driver interface, which is not a public Windows API; emulating it would need knowledge of that interface we may only obtain from outside observation, and running the real IddCx driver would need a UMDF/IddCx host in Wine (far beyond this project). **Not pursued under guardrail 5** (D-019). The public parts on the path (`QueryDisplayConfig`, the `MonitorDataStore` key) are still worth fixing: they are normal Wine work.

**B. Our own virtual entry, through the APIs we already implement (chosen).** The `dxgi.dll` proxy adds a placeholder output to the list Parsec builds its display menu from; duplicating it asks the bridge to create the KWin virtual output; input aimed at its rectangle is redirected to it. Nothing in Parsec changes, and it stays inside public DXGI/user32 behaviour.

## Spikes first (observation only, no client needed unless noted)

| # | Owner | Question | Decides |
|---|---|---|---|
| S1 | eclaireur | Which calls does Parsec use to build the display list it sends the client, and to aim input (`EnumOutputs`/`GetDesc`, `EnumDisplayDevices`, `EnumDisplayMonitors`/`GetMonitorInfo`, `QueryDisplayConfig`, `GetSystemMetrics(SM_*VIRTUALSCREEN)`)? When does it re-enumerate: at connect, on `WM_DISPLAYCHANGE`, when the client switches display? Does it call `ChangeDisplaySettingsEx` for "match client resolution"? `+relay` trace during one real connection (needs the human). | Whether a DXGI-only placeholder is enough, or the user32 patch must also list it; how F1 detects "selected" and "unselected". |
| S2 | capture | `stream_virtual_output` on KWin 6.8.1: does the output show in `kscreen-doctor` and in XWayland's `xrandr`? Where is it placed? Is it removed when the stream closes, and when the process is killed? 60 Hz? Does creating it wake screens that are DPMS-off? Does it write an entry in `~/.config/kwinoutputconfig.json` (a file outside the repo: then ROLLBACK)? Native tool in `src/capture/tools/`. | Lifetime design; whether a ROLLBACK line is needed. |
| S3 | entrees | Does `SendInput` with an absolute point outside Wine's virtual screen reach our LL hook unclamped? Can the uinput absolute device be recreated live with a larger range, or should the virtual output use relative motion / libei? | Coordinates of the placeholder; the input path. |
| S4 | capture | How to tell "screens off": DPMS state per output (`org_kde_kwin_dpms` or `kscreen-doctor --dpms show`), DisplayPort hotplug (the output disappears), zero outputs at all (do XWayland and Wine survive?). A monitor turned off with its button over HDMI usually keeps its EDID and sends no event: probably **undetectable**, to be confirmed and then documented. | F2 trigger. |
| S5 | eclaireur | Can Wine pick up a new XWayland output live (patch 0006 in winex11, its Unix part now built like ntdll.so)? Optional: it would also fix the half-screen offset bug for real, but plan B does not depend on it. | Whether `layout-watch` can be retired later. |

## Design (plan B, adjusted after the spikes)

### Bridge protocol (`src/bridge/proto/`, announced to everyone)
- `PLH_MSG_VOUT_CREATE {width, height, refresh}` → `{output id, KWin rect}`; `PLH_MSG_VOUT_DESTROY {id}`.
- `PLH_MSG_CAP_LIST_OUTPUTS` gains a `PLH_CAP_OUTPUT_VIRTUAL` flag.
- `PLH_MSG_DISPLAY_STATE` → `{real outputs on, real outputs off, virtual active}`.
- `PLH_MSG_INPUT_REDIRECT`: a table *Windows rect → KWin rect* that the input hook applies before the global translation. One mechanism serves F1 and F2.

### capture (`src/bridge/`, `src/capture/`)
- The bridge owns the virtual output. It removes it when (a) Parsec releases the duplication, after a 3 s grace period (Parsec recreates duplications on a resolution change), (b) the client disconnects, (c) the `dxgi.dll` connection drops (Parsec crashed), (d) the bridge exits. KWin's stream ownership covers a bridge crash.
- At most one virtual output; size from the config (default 1920x1080@60), then from the client resolution if S1 shows a usable signal. Width capped at 4096 for H.264 (encodeur's limit).
- Name `plh-virtual`, description "Virtual display (parsec-for-linux)".

### capture, `dxgi.dll` proxy (`src/capture/dxgi/`)
- Wrap `IDXGIAdapter::EnumOutputs` to append a placeholder output when the option is on. Its `GetDesc` gives a rect right of Wine's desktop (or whatever S3 allows) and a device name after the last real one; the monitor name "Virtual display" goes through patch 0005's registry key.
- `DuplicateOutput[1]` on the placeholder → `VOUT_CREATE`, then the normal stream path; `Release` → grace timer → `VOUT_DESTROY`.
- **F2:** if `DISPLAY_STATE` says every real output is off when Parsec duplicates its default output, duplicate the virtual output instead and push a redirect *default output rect → virtual output*. Parsec keeps believing it shows its default screen; the client sees and controls the virtual one.

### entrees (`src/input/`)
- Apply the redirect table in `plh-input-hook` / the bridge input path.
- The uinput absolute range follows the live KWin desktop, which grows by one screen (S3 decides recreate vs relative/libei).
- Input safety: the virtual output moves the outer desktop edge (KDE Connect rule). Edges must come from the live layout, never from the fixed numbers in CLAUDE.md.

### lead (`bin/`, config, tray)
- `layout-watch` ignores outputs named `plh-virtual` (in the comparison and in the saved snapshot), otherwise creating the virtual screen would restart the host.
- Two settings in `plhconfig` and the tray's settings window, both on by default, like Parsec's own host options: "Offer a virtual display" and "Use a virtual display when the screens are off", plus the resolution.
- `status` shows the virtual output when it exists.
- README, PROGRESS, compat-matrix (KWin only at first; GNOME/wlroots have no equivalent protocol in use here: "not tested / not supported").

### encodeur (`src/encode/`)
- Check that the AMF→VA-API path handles sizes other than 1080p and a size change during a session (re-init); report the real limits.

### critique (`tests/`)
- `t40-virtual-display.sh` (`make test`, no visible effect apart from a short-lived extra output; to be confirmed by S2):
  1. native: `VOUT_CREATE` → the output appears in `kscreen-doctor`; `VOUT_DESTROY` → gone; bridge killed → gone;
  2. under Wine: a probe exe sees the placeholder in `EnumOutputs`, duplicates it, receives frames; after `Release` + grace, the output is gone;
  3. `layout-watch` does not restart the host for `plh-virtual` (t04 extension);
  4. F2 with a simulated state (`PLH_FAKE_SCREENS_OFF=1` in the bridge): duplicating the default output yields virtual frames and the redirect table.
- `make test-full`: input on the virtual output (lock, 50 px margins, probe click first); a real DPMS off/on of the screens, restored at the end.
- Adversarial: output left behind after a crash, double creation, flapping between displays, client disconnect during the grace period.
- Human checklist: select "Virtual display" from a real client, switch back, disconnect; connect with the screens off (DPMS, then the button on a DP and an HDMI screen).

## Order and dependencies

1. Spikes S1–S4 in parallel (S1 needs one client connection from the human). S5 in the background.
2. Go/no-go review by the lead and critique: write the outcome in DECISIONS.md, adjust this plan.
3. Protocol header (capture), then in parallel: bridge VOUT (capture), placeholder and F2 redirect in `dxgi.dll` (capture), redirect and uinput range (entrees), `layout-watch` and settings (lead), size checks (encodeur), `t40` written against the protocol from the start (critique).
4. Integration: F1 first, then F2.
5. Real-client trial by the human, then PROGRESS.md with the test commands.

## Risks and exits

- **S1 shows Parsec builds its list from something we cannot extend without a user32/win32u change:** extend patch 0005's area in user32 if possible; otherwise F1 falls back to a tray/config switch that creates the virtual display at the next connection (F2 still works through the redirect).
- **Parsec reacts badly to an output that has no Wine monitor behind it** (a hidden version of the "two identical screens" bug): the redirect-only approach for F2 does not add an output and remains possible on its own.
- **KWin keeps the virtual output in its saved config:** ROLLBACK line plus cleanup in `uninstall.sh` before any build.
- **Screens off over HDMI not detectable:** documented limit; F2 covers DPMS and disconnected screens.
- **KWin only:** other desktops get "not supported" in the compat matrix until an equivalent exists.
