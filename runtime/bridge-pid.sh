# shellcheck shell=bash
# Identification of the HOST plh-bridge (the one serving $XDG_RUNTIME_DIR/plh-bridge), as opposed to the private
# test bridges of the teammates (--runtime-dir DIR / PLH_BRIDGE_DIR). Source it: . runtime/bridge-pid.sh
#
#   plh_bridge_pidfile            path of the PID file ($XDG_RUNTIME_DIR/plh-bridge-host.pid, outside the bridge
#                                 directory because the bridge owns/cleans that one)
#   plh_host_bridge_pid           prints the PID of the host bridge, exit 1 if none. Validated: the PID file's
#                                 process must be named plh-bridge and have no --runtime-dir / PLH_BRIDGE_DIR.
#   plh_host_bridge_record PID    called by run-parsec.sh after launching the host bridge
#   plh_host_bridge_stop          stops only the host bridge (SIGTERM, then SIGKILL after ~3 s)
#   plh_stray_bridges             prints the PIDs of plh-bridge processes that use the host directory without
#                                 being the recorded host bridge (started without --runtime-dir/PLH_BRIDGE_DIR)

plh_bridge_pidfile() { echo "${XDG_RUNTIME_DIR:-/tmp}/plh-bridge-host.pid"; }

# 0 if PID is a plh-bridge on the host directory (no private --runtime-dir, no PLH_BRIDGE_DIR)
_plh_is_host_style_bridge() {
    local pid=$1 comm args
    [ -r "/proc/$pid/comm" ] || return 1
    read -r comm < "/proc/$pid/comm" 2>/dev/null || return 1
    [ "$comm" = plh-bridge ] || return 1
    args=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null) || return 1
    case " $args " in *" --runtime-dir"*|*" -r "*) return 1 ;; esac
    tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -q '^PLH_BRIDGE_DIR=.' && return 1
    return 0
}

plh_host_bridge_pid() {
    local pid f; f=$(plh_bridge_pidfile)
    [ -f "$f" ] || return 1
    pid=$(cat "$f" 2>/dev/null)
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    _plh_is_host_style_bridge "$pid" || return 1
    echo "$pid"
}

plh_host_bridge_record() { echo "$1" > "$(plh_bridge_pidfile)"; }

plh_stray_bridges() {
    local d pid host; host=$(plh_host_bridge_pid 2>/dev/null || true)
    for d in /proc/[0-9]*; do
        pid=${d#/proc/}
        [ "$pid" = "$host" ] && continue
        [ "$(stat -c %u "$d" 2>/dev/null)" = "$(id -u)" ] || continue
        _plh_is_host_style_bridge "$pid" && echo "$pid"
    done
    return 0
}

plh_host_bridge_stop() {
    local pid i; pid=$(plh_host_bridge_pid) || return 0
    kill "$pid" 2>/dev/null || true
    for i in 1 2 3 4 5 6; do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
    rm -f "$(plh_bridge_pidfile)"
    return 0
}
