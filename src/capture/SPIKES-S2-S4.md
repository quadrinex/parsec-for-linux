# Spikes S2 and S4 (virtual display plan) — capture, 2026-11-08

Tool: `build/bridge/vout-probe` (`make -C src/bridge ROOT=$PWD $PWD/build/bridge/vout-probe`, source `src/capture/tools/vout-probe.c`).
Options: `-w -h` size, `-t` seconds, `-n` name, `-d` plain `stream_virtual_output`, `-k` hold until killed.
KWin 6.8.1, zkde_screencast v6.

## S2 (all PROVEN by the runs below unless marked inferred)

| Question | Result |
|---|---|
| Output appears? | Yes. `kscreen-doctor -o`: `Output: 1 Virtual-plh-virtual <uuid>`, priority 4, mode `1920x1080@60.00*!`, Geometry `5760,0 1920x1080`, scale 1. Other outputs get renumbered (indices shift). |
| XWayland | `xrandr --listmonitors`: `3: +Virtual-plh-virtual 1920/1920x1080/1080+5760+0` (4 monitors instead of 3). Name = `Virtual-` + requested name. Position = right of the rightmost screen. |
| Primary changed? | No (DP-2 stays `*`). |
| Frames | Stream goes streaming with format BGRx 1920x1080, framerate 0/1 (variable), max 60/1. Frames are **damage driven**: an idle virtual output delivered 3-14 frames in the first ~0.47 s then nothing (4 / 14 / 3 frames in three runs, last frame always ~470 ms after start). Real fps under load NOT measured (needs content on the output; nothing moved onto it). |
| Removed on stream close | Yes: after `close` + exit, `xrandr` and `kscreen-doctor` show no Virtual output (checked right after exit). |
| Removed on SIGKILL | Yes: 1.5 s after `kill -KILL`, 0 Virtual entries in both. |
| Non-1080p | `-w 2560 -h 1440` works: output `2560/2560x1440/1440+5760+0`, PipeWire format 2560x1440. |
| `stream_virtual_output` (no description) | Works the same (`-d`). |
| Windows moved onto it | No window was moved by KWin when the output appeared (geometry of real outputs unchanged). Windows were not enumerated individually: inferred. |
| Wake DPMS-off screens | NOT tested (screens were on). Human experiment below. |

### ~/.config/kwinoutputconfig.json — YES, KWin writes it (needs a ROLLBACK / uninstall decision by lead)
Copy before: `/tmp/plh-capture/kwinoutputconfig.before.json`. The file was modified (mtime changes during runs). Diff after 5 runs (52 changed lines):
- the `outputs` list entry for the virtual output is **rewritten each time**: `"connectorName": "Virtual-Virtuel"` (left by an earlier virtual-output use, before this spike) became `"Virtual-plh-virtual"`, and is now `"Virtual-plh-virtual2"` (last run); plus `"maxBitsPerColor": 0`;
- one new entry in `setups` (a 4-output layout with the virtual output at `x=5760`), i.e. KWin remembers the 4-screen setup. It is keyed by outputs, so it is only used when the same set of outputs exists again.
The real-output entries are unchanged. I did not edit or restore the file. The virtual entry is bounded (one entry, overwritten), but the extra `setups` item persists: not harmful, only a stale layout.

### Side effects seen
- `bin/parsec-linux-host stop` then `start` done around the runs (no client was connected); the host is running again (status verified). During the runs layout-watch was stopped, so its reaction to the new output was NOT observed (the plan's `plh-virtual` exclusion is still needed: xrandr lists it as a 4th monitor).

## S4

| Question | Result |
|---|---|
| DPMS per output | PROVEN readable: `kscreen-doctor --dpms show` prints `dpms mode for screen DP-2: on` (also HDMI-A-1, HDMI-A-2). Protocol: `org_kde_kwin_dpms_manager` v1 is advertised (`wayland-info`); per output it gives `supported`, `mode`, `done` events (the source of kscreen-doctor's answer). Also advertised: `ext_idle_notifier_v2`, `org_kde_kwin_idle`, `kde_output_device_registry_v2`. |
| Kernel view (read-only) | `/sys/class/drm/card1-*/{status,dpms,enabled}` readable without privileges: `card1-DP-2 connected dpms=On enabled=enabled`, `card1-DP-1 disconnected dpms=Off enabled=disabled`. Useful second source; works when KWin DPMS is not exposed. |
| DP hotplug | Inferred (not tested): a DP monitor that is switched off/unplugged drops HPD, the connector goes `disconnected` and KWin removes the output (`kde_output_device` removed, wl_output global removed, XWayland RRScreenChange). A DP monitor in standby may instead keep HPD. HDMI monitors turned off by their button usually stay `connected` with EDID (plan's "probably undetectable"). |
| Zero outputs | Inferred only, not tested: KWin keeps running with no real output (it creates a fallback/virtual placeholder only with no output at all in recent versions); XWayland keeps a screen but with no RandR monitors, Wine's winex11 then falls back to a default monitor. Behaviour of our patched winex11 and DXVK in this case is UNKNOWN. |

### Experiments the human must do (S4 and S2 leftover) — not done by me
Run each with the host stopped or no client connected, one at a time, screens restored at the end. Record `kscreen-doctor --dpms show`, `kscreen-doctor -o | grep -c Output`, `xrandr --listmonitors`, `for c in /sys/class/drm/card1-*/; do echo $c $(cat $c/status $c/dpms); done`, before / during / after:
1. `kscreen-doctor --dpms off` (or system DPMS timeout): expect dpms=off per output; do outputs stay in `kscreen-doctor -o`?
2. Power button of ONE monitor on DP-2, then ONE on HDMI: does `status` go `disconnected` (output removed) or stay `connected`; does `--dpms show` change?
3. While DPMS is off: `vout-probe -t 5` — do the real screens wake (`--dpms show` goes back to `on`)? do frames arrive? Is the virtual output removed at the end and do the screens stay off?
4. Ask the human for one real-content test: drag a window onto the virtual output during `vout-probe -k` (kill it after) to measure fps under damage.
