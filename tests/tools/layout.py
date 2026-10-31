#!/usr/bin/env python3
"""Compares the Windows layout ('monitor ... rect=' lines of hello.exe or
'output ... rect=' of dda_capture.exe, on stdin) with KWin's
(kscreen-doctor -j). Windows forces the primary screen at 0,0: we therefore require
one common translation (dx,dy) for all screens, not the raw KWin coordinates.
Output: one line 'map <windows_name> <connector> <left_to_right_rank>' per
screen, then 'translation dx,dy'. Exit 1 if inconsistent."""
import json, re, subprocess, sys

k = [o for o in json.loads(subprocess.check_output(["kscreen-doctor", "-j"]))["outputs"] if o["enabled"]]
k.sort(key=lambda o: (o["pos"]["x"], o["pos"]["y"]))
kw = {o["name"]: (o["pos"]["x"], o["pos"]["y"], o["size"]["width"], o["size"]["height"], i, o.get("priority"))
      for i, o in enumerate(k)}
win = []
for line in sys.stdin:
    m = re.search(r"name=(\S+) rect=(-?\d+),(-?\d+),(\d+),(\d+)", line)
    if m and line.startswith(("monitor", "output")):
        idx = re.search(r"idx=(\d+)", line)
        win.append((idx.group(1) if idx else m.group(1), m.group(1), *map(int, m.groups()[1:]),
                    "primary=1" in line))
errs = []
if len(win) != len(kw):
    errs.append(f"{len(win)} screens on the Windows side, {len(kw)} on the KWin side")
prim = [w for w in win if w[6]]
kprim = [n for n, v in kw.items() if v[5] == 1]
# translation: preferably via the primary screen, otherwise via the leftmost
if win:
    ref = min(win, key=lambda w: (w[2], w[3]))
    kref = k[0]
    dx, dy = ref[2] - kref["pos"]["x"], ref[3] - kref["pos"]["y"]
    for w in win:
        hit = [n for n, v in kw.items() if (v[0] + dx, v[1] + dy, v[2], v[3]) == tuple(w[2:6])]
        if not hit:
            errs.append(f"{w[1]} rect={w[2]},{w[3]},{w[4]},{w[5]} without matching KWin screen (translation {dx},{dy})")
        else:
            print(f"map {w[0]} {w[1]} {hit[0]} {kw[hit[0]][4]}")
            if w[6] and kprim and hit[0] != kprim[0]:
                errs.append(f"Windows primary = {hit[0]}, KWin primary = {kprim[0]}")
    print(f"translation {dx},{dy}")
for e in errs:
    print("error", e)
sys.exit(1 if errs else 0)
