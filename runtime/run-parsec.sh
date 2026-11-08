#!/usr/bin/env bash
# Runs the official Parsec Windows app in the dedicated prefix, in user mode
# (parsecd.exe directly, not through the pservice.exe service).
# Usage: runtime/run-parsec.sh [--trace CHANNELS] [--fg] [--] [parsecd args]
#   --trace CHANNELS : WINEDEBUG (e.g. +loaddll,+seh); log -> runtime/traces/parsec-<date>.log
#   --full           : grouped restart: amfrt64 (install-amf.sh), clipboard gateway (plh-clipd + plh-clipsync.exe),
#                      plh-input-hook.exe, bridge restarted with PLH_INPUT_BACKEND=uinput
#   --d3d-trace      : PLH_D3D_TRACE=1 (D3D11 tracer of the dxgi proxy)
#   --fg             : foreground (default: background, PID in runtime/traces/parsec.pid)
# Variables: PLH_DLLOVERRIDES (appended to WINEDLLOVERRIDES: project DLLs, prefix only).
set -euo pipefail
. "$(dirname "$0")/env.sh"

TRACE="" FG=0 FULL=0 D3DT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --trace) TRACE=$2; shift 2;;
        --fg) FG=1; shift;;
        --full) FULL=1; shift;;
        --d3d-trace) D3DT=1; shift;;
        --) shift; break;;
        *) break;;
    esac
done

PARSECD="$WINEPREFIX/drive_c/Program Files/Parsec/parsecd.exe"
[ -f "$PARSECD" ] || { echo "Parsec not installed: run runtime/install-parsec.sh" >&2; exit 1; }

# The Parsec service (pservice.exe, LocalSystem) is not used: manual start only.
"$WINE" reg add 'HKLM\System\CurrentControlSet\Services\Parsec' /v Start /t REG_DWORD /d 3 /f >/dev/null 2>&1 || true

[ -n "${PLH_DLLOVERRIDES:-}" ] && export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;$PLH_DLLOVERRIDES"
export WINEDEBUG="${TRACE:-${WINEDEBUG:--all}}"
# Focused trace set up by runtime/trace-display-list.sh (file removed by its --restore).
[ -f "$PLH_TRACES/wine-debug.override" ] && export WINEDEBUG="$(cat "$PLH_TRACES/wine-debug.override")"
# DXVK: HUD off, log in traces
export DXVK_LOG_PATH="$PLH_TRACES" DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"

# dxgi proxy (idempotent) + plh-bridge daemon (KWin capture) if not running.
ROOT_PLH="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "$ROOT_PLH/build/dxgi/dxgi.dll" ]; then "$ROOT_PLH/src/capture/dxgi/install-proxy.sh" >/dev/null; fi
[ "$D3DT" = 1 ] && export PLH_D3D_TRACE=1
if [ "$FULL" = 1 ]; then
    # the bridge must be (re)started with the input backend; otherwise leave it as is
    pgrep -x plh-bridge >/dev/null && { pkill -x plh-bridge || true; sleep 1; }
    if [ "${PLH_ENABLE_INPUT:-1}" = 1 ]; then
        export PLH_INPUT_BACKEND="${PLH_INPUT_BACKEND:-uinput}"
    else
        export PLH_INPUT_BACKEND=off
    fi
    if [ "${PLH_ENABLE_AMF:-1}" = 1 ]; then
        [ -f "$ROOT_PLH/build/encode/amf/amfrt64.dll" ] && "$ROOT_PLH/src/encode/amf/install-amf.sh" >/dev/null
    else
        rm -f "$WINEPREFIX/drive_c/windows/system32/amfrt64.dll"
    fi
fi
if [ -x "$ROOT_PLH/build/bridge/plh-bridge" ] && ! pgrep -x plh-bridge >/dev/null; then
    BRIDGE_ARGS=(--capture-backend "${PLH_CAPTURE_BACKEND:-kwin}")
    [ "${PLH_BRIDGE_VERBOSE:-0}" = 1 ] && BRIDGE_ARGS+=(-v)
    [ -n "${PLH_BRIDGE_PORT:-}" ] && [ "${PLH_BRIDGE_PORT:-0}" != 0 ] && BRIDGE_ARGS+=(--port "$PLH_BRIDGE_PORT")
    nohup "$ROOT_PLH/build/bridge/plh-bridge" "${BRIDGE_ARGS[@]}" >"$PLH_TRACES/plh-bridge.log" 2>&1 &
    sleep 1
fi
if [ "$FULL" = 1 ]; then
    if [ "${PLH_ENABLE_CLIPBOARD:-1}" = 1 ]; then
        # clipboard gateway (text): Linux half, then PE half in the prefix
        CLIPDIR="$XDG_RUNTIME_DIR/plh-bridge/clip"
        pgrep -f plh-clipd.sh >/dev/null || nohup "$ROOT_PLH/src/clipboard/plh-clipd.sh" >"$PLH_TRACES/plh-clipd.log" 2>&1 &
        mkdir -p "$ROOT_PLH/build/clipboard"
        x86_64-w64-mingw32-gcc -std=c11 -include string.h -o "$ROOT_PLH/build/clipboard/plh-clipsync.exe" "$ROOT_PLH/src/clipboard/plh-clipsync.c"
        sleep 1
        WINEDEBUG=-all nohup "$WINE" "$ROOT_PLH/build/clipboard/plh-clipsync.exe" "$CLIPDIR" >"$PLH_TRACES/plh-clipsync.log" 2>&1 &
    fi
    if [ "${PLH_ENABLE_INPUT:-1}" = 1 ]; then
        # relay of the inputs injected by Parsec (low-level hooks -> plh-bridge service 0x03)
        [ -f "$ROOT_PLH/build/input/pe/plh-input-hook.exe" ] && \
            WINEDEBUG=-all nohup "$WINE" "$ROOT_PLH/build/input/pe/plh-input-hook.exe" >"$PLH_TRACES/plh-input-hook.log" 2>&1 &
    fi
    if [ "${PLH_ENABLE_GAMEPAD:-1}" = 1 ] && [ -f "$ROOT_PLH/build/input/pe/plh-pad-relay.exe" ] && [ -f "$ROOT_PLH/build/input/pe/plh-vigem.sys" ]; then
        # gamepads: ViGEmBus-compatible driver in the prefix + relay to plh-bridge; the bus interface
        # must be published before parsecd starts (its keys are volatile, redone at every start)
        WINEDEBUG=-all nohup "$WINE" "$ROOT_PLH/build/input/pe/plh-pad-relay.exe" --install "$ROOT_PLH/build/input/pe/plh-vigem.sys" >"$PLH_TRACES/plh-pad-relay.log" 2>&1 &
        for _ in $(seq 300); do grep -q READY "$PLH_TRACES/plh-pad-relay.log" 2>/dev/null && break; sleep 0.2; done
    fi
fi

# One tray icon: bin/parsec-tray replaces Wine's blank item (read by explorer.exe at its next start).
"$WINE" reg add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer' /v NoTrayItemsDisplay /t REG_DWORD /d 1 /f >/dev/null 2>&1 </dev/null || true

# Real monitor names for EnumDisplayDevices (patch 0005)
NAMES="$("$ROOT_PLH/runtime/monitor-names.py" 2>/dev/null || true)"
if [ -n "$NAMES" ]; then
    "$WINE" reg delete 'HKLM\Software\Wine\PlhMonitorNames' /f >/dev/null 2>&1 </dev/null || true
    while IFS=$'\t' read -r pos name; do
        "$WINE" reg add 'HKLM\Software\Wine\PlhMonitorNames' /v "$pos" /d "$name" /f >/dev/null 2>&1 </dev/null || true
    done <<<"$NAMES"
fi

LOG="$PLH_TRACES/parsec-$(date +%Y%m%d-%H%M%S).log"
ln -sfn "$(basename "$LOG")" "$PLH_TRACES/parsec-latest.log"
echo ">> parsecd.exe ; Wine log: $LOG ; Parsec log: $WINEPREFIX/drive_c/users/$USER/AppData/Roaming/Parsec/log.txt"
cd "$WINEPREFIX/drive_c/Program Files/Parsec"
if [ "$FG" = 1 ]; then
    exec "$WINE" parsecd.exe "$@" >"$LOG" 2>&1
else
    nohup "$WINE" parsecd.exe "$@" >"$LOG" 2>&1 &
    echo $! > "$PLH_TRACES/parsec.pid"
    echo ">> PID $(cat "$PLH_TRACES/parsec.pid")"
fi
