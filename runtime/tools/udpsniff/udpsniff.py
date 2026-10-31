#!/usr/bin/env python3
"""Passive sniffer (AF_PACKET, read-only, root required): summarizes the UDP datagrams of the Parsec port.
Usage: sudo python3 -I udpsniff.py PORT SECONDS OUTPUT. Keeps no payload and no identifiers:
time, interface, direction, addresses, length, STUN type if any."""
import socket, struct, sys, time
port, secs, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3)); s.settimeout(1)
seen = {}; end = time.time() + secs; f = open(out, "w", buffering=1)
def stun(p):
    if len(p) >= 20 and p[4:8] == b"\x21\x12\xa4\x42":
        return "STUN type=0x%04x" % struct.unpack("!H", p[:2])[0]
    return ""
while time.time() < end:
    try: pkt, meta = s.recvfrom(65535)
    except socket.timeout: continue
    if len(pkt) < 14: continue
    et = struct.unpack("!H", pkt[12:14])[0]; b = pkt[14:]
    if et == 0x0800 and len(b) >= 28 and b[9] == 17:
        ihl = (b[0] & 15) * 4; sp, dp, ln = struct.unpack("!HHH", b[ihl:ihl+6]); a, d = socket.inet_ntoa(b[12:16]), socket.inet_ntoa(b[16:20])
    elif et == 0x86dd and len(b) >= 48 and b[6] == 17:
        sp, dp, ln = struct.unpack("!HHH", b[40:46]); a, d = socket.inet_ntop(socket.AF_INET6, b[8:24]), socket.inet_ntop(socket.AF_INET6, b[24:40])
        ihl = 40
    else: continue
    if port in (sp, dp):
        st = stun(b[ihl+8:ihl+48]); k = (meta[0], a, sp, d, dp)
        if not st:                      # data flow: first trace of each flow only
            if k in seen: continue
            seen[k] = 1
        f.write("%s %s %s:%d -> %s:%d len=%d %s\n" % (time.strftime("%H:%M:%S"), meta[0], a, sp, d, dp, ln - 8, st))
