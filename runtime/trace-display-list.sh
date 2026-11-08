#!/usr/bin/env bash
# Focused +relay trace of the display-related calls Parsec makes, for ONE client session
# (spike S1 of docs/plan-virtual-display.md). Observation only; nothing in Parsec is touched.
#
#   runtime/trace-display-list.sh [run]   restart the host with the trace, wait for one client
#                                         session (connect ... disconnect), then restore the
#                                         normal host. Ctrl-C also restores.
#   runtime/trace-display-list.sh start   restart the host with the trace and return
#   runtime/trace-display-list.sh --restore   remove the trace settings, restart the host normally
#   Options: --max-minutes N (run: give up and restore after N minutes, default 30)
#
# Refuses to restart the host while a client is connected (it would cut the session).
# What it changes (dedicated prefix and runtime/traces only):
#   - HKCU\Software\Wine\Debug\RelayInclude (prefix registry value only; the other Wine defaults stay), deleted by --restore
#   - runtime/traces/wine-debug.override (read by run-parsec.sh), deleted by --restore
# Output: runtime/traces/parsec-<date>.log, linked as display-list-latest.log (WINEDEBUG=+relay,+loaddll, filtered by RelayInclude)
set -u
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
. "$ROOT/runtime/env.sh"
HOST="$ROOT/bin/parsec-linux-host"
OVERRIDE="$PLH_TRACES/wine-debug.override"
PARSEC_LOG="$WINEPREFIX/drive_c/users/$USER/AppData/Roaming/Parsec/log.txt"
MAXMIN=30

# user32/win32u display + cursor calls, DXVK enumeration path (what feeds the Parsec display list),
# SetupAPI/cfgmgr32/registry/CreateFile/DeviceIoControl for the virtual display init (MM_Connect).
INCLUDE="user32.EnumDisplayDevicesW;user32.EnumDisplayDevicesA;user32.EnumDisplayMonitors;\
user32.GetMonitorInfoW;user32.GetMonitorInfoA;user32.EnumDisplaySettingsW;user32.EnumDisplaySettingsA;\
user32.EnumDisplaySettingsExW;user32.ChangeDisplaySettingsW;user32.ChangeDisplaySettingsExW;\
user32.GetSystemMetrics;user32.GetSystemMetricsForDpi;user32.MonitorFromPoint;user32.MonitorFromWindow;\
user32.MonitorFromRect;user32.QueryDisplayConfig;user32.GetDisplayConfigBufferSizes;\
user32.DisplayConfigGetDeviceInfo;user32.DisplayConfigSetDeviceInfo;user32.SetDisplayConfig;\
user32.SetCursorPos;user32.ClipCursor;user32.SendInput;user32.GetDpiForMonitor;user32.SetThreadDpiAwarenessContext;\
shcore.GetDpiForMonitor;shcore.GetScaleFactorForMonitor;\
setupapi.SetupDiGetClassDevsW;setupapi.SetupDiGetClassDevsA;setupapi.SetupDiEnumDeviceInterfaces;\
setupapi.SetupDiGetDeviceInterfaceDetailW;setupapi.SetupDiEnumDeviceInfo;setupapi.SetupDiOpenDeviceInfoW;\
setupapi.SetupDiCreateDeviceInfoList;setupapi.SetupDiCallClassInstaller;\
cfgmgr32.CM_Get_Device_Interface_ListW;cfgmgr32.CM_Get_Device_Interface_List_SizeW;cfgmgr32.CM_Locate_DevNodeW;\
cfgmgr32.CM_Register_Notification;cfgmgr32.CM_Reenumerate_DevNode_Ex;\
setupapi.CM_Get_Device_Interface_ListW;setupapi.CM_Get_Device_Interface_List_SizeW;\
kernelbase.CreateFileW;kernelbase.CreateFileA;kernelbase.DeviceIoControl;\
kernelbase.RegOpenKeyExW;kernelbase.RegOpenKeyExA;kernelbase.RegCreateKeyExW"

die() { echo "trace-display-list: $*" >&2; exit 1; }

client_connected() {
    [ -f "$PARSEC_LOG" ] || return 1
    tr -d '\r' <"$PARSEC_LOG" | grep -aE ' (dis)?connected\.$' | tail -1 | grep -q ' connected\.$' \
        && pgrep -f 'parsecd\.exe' >/dev/null
}

tracing_on() { [ -f "$OVERRIDE" ]; }

set_relay_lists() {
    WINEDEBUG=-all "$WINE" reg add 'HKCU\Software\Wine\Debug' /v RelayInclude /d "$INCLUDE" /f >/dev/null 2>&1 </dev/null
}
clear_relay_lists() {
    WINEDEBUG=-all "$WINE" reg delete 'HKCU\Software\Wine\Debug' /v RelayInclude /f >/dev/null 2>&1 </dev/null || true
}

restart_host() { "$HOST" stop; sleep 2; "$HOST" start; }

start_trace() {
    client_connected && die "a client is connected: not restarting the host (run again after it disconnects)"
    if "$HOST" status 2>/dev/null | grep -q '^client .*connected$'; then
        die "bin/parsec-linux-host status reports a connected client"
    fi
    "$HOST" stop
    sleep 2
    set_relay_lists || die "could not write the relay lists in the prefix registry"
    echo "+relay,+loaddll" >"$OVERRIDE"
    # run-parsec.sh names its own log parsec-<date>.log and links it to parsec-latest.log
    "$HOST" start
    sleep 3
    LATEST="$(readlink -f "$PLH_TRACES/parsec-latest.log")"
    ln -sfn "$(basename "$LATEST")" "$PLH_TRACES/display-list-latest.log"
    echo ">> trace log: $LATEST (also runtime/traces/display-list-latest.log)"
}

restore() {
    local waited=0
    while client_connected; do
        [ $waited -eq 0 ] && echo ">> a client is still connected: waiting for it to disconnect before restoring the normal host"
        waited=1; sleep 3
    done
    rm -f "$OVERRIDE"
    "$HOST" stop
    sleep 2
    clear_relay_lists
    "$HOST" start
    echo ">> normal host restored (no trace)"
    [ -n "${LATEST:-}" ] && ls -la "$LATEST" | awk '{print ">> trace size: " $5 " bytes"}'
}

cmd="${1:-run}"
case "$cmd" in
    --restore|restore) restore; exit 0 ;;
    start) start_trace; exit 0 ;;
    run|--run|"")
        [ "${2:-}" = --max-minutes ] && MAXMIN="${3:-30}"
        start_trace
        trap 'echo; echo ">> interrupted"; restore; exit 130' INT TERM
        trap 'tracing_on && restore' EXIT
        echo ">> host is tracing. Now: connect ONE Parsec client, switch display once, disconnect."
        echo ">> (waiting up to $MAXMIN min; the host is restored automatically after the disconnect)"
        end=$(( $(date +%s) + MAXMIN * 60 )); seen=0
        while [ "$(date +%s)" -lt "$end" ]; do
            if client_connected; then seen=1; elif [ $seen -eq 1 ]; then
                echo ">> client disconnected"; sleep 5; break
            fi
            sleep 2
        done
        [ $seen -eq 1 ] || echo ">> no client within $MAXMIN min"
        exit 0 ;;
    *) die "usage: $0 [run [--max-minutes N]|start|--restore]" ;;
esac
