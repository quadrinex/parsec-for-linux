#!/usr/bin/env python3
"""Local attacks against a PRIVATE plh-bridge instance (never the user's).
  bridge_attack.py <runtime_dir> <case>
Cases: garbage | hugesize | prehello | badtoken | flood N | halfmsg
Prints a result line; exit code 0 if the behavior is safe, 1 otherwise."""
import os, resource, socket, struct, sys, time

rd, case = sys.argv[1], sys.argv[2]
port = int(open(os.path.join(rd, "port")).read().strip())
MAGIC = 0x42484C50


def conn():
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    return s


def closed_by_peer(s, wait=2.0):
    s.settimeout(wait)
    try:
        while True:
            d = s.recv(65536)
            if not d:
                return True
    except (ConnectionResetError, BrokenPipeError):
        return True
    except socket.timeout:
        return False


def hdr(t, size, seq=1, flags=0):
    return struct.pack("<IHHII", MAGIC, t, flags, size, seq)


if case == "garbage":
    s = conn(); s.sendall(os.urandom(4096)); r = closed_by_peer(s)
    print(f"garbage: connection closed={r}"); sys.exit(0 if r else 1)
if case == "hugesize":
    s = conn(); s.sendall(hdr(1, 0xFFFFFFFF)); r = closed_by_peer(s)
    print(f"size 0xFFFFFFFF: connection closed={r}"); sys.exit(0 if r else 1)
if case == "prehello":
    s = conn(); s.sendall(hdr(3, 8) + b"x" * 8); r = closed_by_peer(s)
    print(f"PING before HELLO: connection closed={r}"); sys.exit(0 if r else 1)
if case == "badtoken":
    s = conn()
    hello = struct.pack("<HHIII", 1, 0, 6, os.getpid(), 0) + os.urandom(32) + b"attack".ljust(64, b"\0")
    s.sendall(hdr(1, len(hello)) + hello)
    s.settimeout(2); d = s.recv(4096)
    st = struct.unpack("<i", d[16:20])[0] if len(d) >= 20 else None
    r = closed_by_peer(s)
    print(f"wrong token: status={st} closed={r} response_bytes={len(d)}"); sys.exit(0 if (st == -3 and r) else 1)
if case == "halfmsg":
    s = conn(); s.sendall(hdr(1, 112) + b"\0" * 10); s.close(); print("half message then close: sent"); sys.exit(0)
if case == "flood":
    n = int(sys.argv[3]); hold = float(sys.argv[4]) if len(sys.argv) > 4 else 5
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, n + 256), hard))
    socks = []
    for _ in range(n):
        try:
            x = socket.socket(); x.setblocking(True); x.settimeout(1); x.connect(("127.0.0.1", port)); socks.append(x)
        except OSError as e:
            print(f"flood: failed at connection {len(socks)+1} : {e!r}", flush=True)
            break
    print(f"flood: {len(socks)} idle unauthenticated TCP connections opened, held {hold}s", flush=True)
    time.sleep(hold)
    for x in socks: x.close()
    sys.exit(0)
