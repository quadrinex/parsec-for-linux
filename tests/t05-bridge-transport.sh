#!/usr/bin/env bash
# Independent reproduction of D-005 (PE<->native transport), under the project's
# REAL Wine (the prefix's wine-staging; D-005 was measured with the system wine):
#   - AF_UNIX winsock (socket + connect to a native socket by Z: path):
#     observed and reported, not required (wine-staging: OK; system wine: 10047);
#   - TCP 127.0.0.1: round trip p99 < 1 ms;
#   - /dev/shm via Z: + CreateFileMapping: 300 complete 1080p frames intact
#     in each direction, delivery < 8 ms per frame (half a frame at 60 Hz).
. "$(dirname "$0")/lib/common.sh"
srv="$BUILD/native/xport_srv"; [ -x "$srv" ] || fail "xport_srv not built"
exe=$(pe_bin xport_probe) || exit $?
need_prefix
d="$TMPD/xport"; rm -rf "$d"; mkdir -p "$d"
"$srv" "$d" >"$d/srv.log" 2>&1 &
sp=$!
for _ in $(seq 50); do [ -s "$d/port" ] && [ -s "$d/shm" ] && break; sleep 0.1; done
[ -s "$d/port" ] || { kill $sp; fail "native server not started : $(cat "$d/srv.log")"; }
# native AF_UNIX listener: the PE tries a connect + sends 4 bytes (information)
python3 -c '
import socket,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.bind(sys.argv[1]); s.listen(1); s.settimeout(60)
c,_=s.accept(); print("native AF_UNIX received", c.recv(16))' "$d/u.sock" >"$d/unix.log" 2>&1 &
up=$!
perm=$(stat -c %a "/dev/shm/$(cat "$d/shm")"); info "shm file permissions : $perm"
out=$(PE_TIMEOUT=120 run_pe "$exe" "$(cat "$d/port")" "Z:\\dev\\shm\\$(cat "$d/shm")" "Z:${d//\//\\}\\u.sock" 2>"$d/pe.err"); rc=$?
kill $up 2>/dev/null; wait $up 2>/dev/null; info "$(cat "$d/unix.log")"
wait $sp; src=$?
echo "$out" | sed 's/^/  # /'; sed 's/^/  # /' "$d/srv.log"
[ $rc = 0 ] && [ $src = 0 ] || fail "PE rc=$rc, native rc=$src ($(tail -2 "$d/pe.err" | tr '\n' ' '))"
p99=$(echo "$out" | sed -n 's/^tcp_rtt_us.*p99=\([0-9.]*\).*/\1/p')
awk -v p="${p99:-1e9}" 'BEGIN{exit !(p<1000)}' || fail "TCP round trip p99=${p99} µs ≥ 1 ms"
for dir in 'native->PE' 'PE->native'; do
  ms=$(echo "$out" | grep "^shm $dir" | sed -n 's/.*ms_per_frame=\([0-9.]*\).*/\1/p')
  awk -v m="${ms:-1e9}" 'BEGIN{exit !(m<8)}' || fail "delivery $dir ${ms} ms/frame ≥ 8 ms"
done
pass "TCP p99=${p99} µs ; 1080p frames intact in both directions via /dev/shm"
