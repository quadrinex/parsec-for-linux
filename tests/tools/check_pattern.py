#!/usr/bin/env python3
"""Checks that an image (PAM/PPM RGB 8-bit) contains the test pattern of pattern.py.

  check_pattern.py IMAGE [--crop X,Y,W,H] [--expect-rank R]

Without --expect-rank: decodes the rank from the identity bar and checks that
the background color matches. Output: "rank=R ok" (exit 0) or the discrepancies
(exit 1). Entirely black image → exit 2 (distinct, frequent case).
Standard library only (no numpy/PIL).
"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from pattern import ANIM, BAR_X0, BAR_Y, BG, CORNERS, NBITS, SQ  # noqa: E402

TOL = 48


def read_pnm(path):
    data = open(path, "rb").read()
    if data.startswith(b"P7"):
        end = data.index(b"ENDHDR\n") + 7
        hdr = dict(l.split(b" ", 1) for l in data[3:end - 7].split(b"\n") if b" " in l)
        w, h, d = int(hdr[b"WIDTH"]), int(hdr[b"HEIGHT"]), int(hdr[b"DEPTH"])
        return w, h, d, data[end:]
    if data.startswith(b"P6"):
        parts, i = [], 2
        while len(parts) < 3:
            while data[i:i + 1].isspace():
                i += 1
            if data[i:i + 1] == b"#":
                i = data.index(b"\n", i)
                continue
            j = i
            while not data[j:j + 1].isspace():
                j += 1
            parts.append(int(data[i:j]))
            i = j
        return parts[0], parts[1], 3, data[i + 1:]
    raise SystemExit(f"unsupported format: {path} (PAM P7 or PPM P6 expected)")


def main():
    a = sys.argv[1:]
    path = a[0]
    w, h, d, px = read_pnm(path)
    cx, cy, cw, ch = 0, 0, w, h
    if "--crop" in a:
        cx, cy, cw, ch = map(int, a[a.index("--crop") + 1].split(","))
    exp = int(a[a.index("--expect-rank") + 1]) if "--expect-rank" in a else None

    def at(x, y):
        o = ((cy + y) * w + (cx + x)) * d
        return tuple(px[o:o + 3])

    def near(c, ref):
        return all(abs(c[i] - ref[i]) <= TOL for i in range(3))

    # Image noire ?
    samples = [at((i * 7919) % cw, (i * 104729) % ch) for i in range(5000)]
    if max(max(s) for s in samples) < 16:
        print("image noire")
        return 2
    errs = []
    half = SQ // 2
    corners = {"tl": (half, half), "tr": (cw - half, half), "bl": (half, ch - half), "br": (cw - half, ch - half)}
    for k, (x, y) in corners.items():
        c = at(x, y)
        if not near(c, CORNERS[k]):
            errs.append(f"corner {k} ({x},{y}) = {c}, expected {CORNERS[k]}")
    v = 0
    for k in range(NBITS):
        c = at(BAR_X0 + SQ * k + half, BAR_Y + half)
        if near(c, (255, 255, 255)):
            v |= 1 << k
        elif not near(c, (0, 0, 0)):
            errs.append(f"bit {k} = {c} neither white nor black")
    rank = v - 1
    bg_pt = (cw - 300, ch // 2)  # outside corners, bar, text and animated area
    assert not (ANIM[0] <= bg_pt[0] < ANIM[0] + ANIM[2] and ANIM[1] <= bg_pt[1] < ANIM[1] + ANIM[3])
    bg = at(*bg_pt)
    if not 0 <= rank < len(BG):
        errs.append(f"invalid decoded rank : {rank}")
    elif not near(bg, BG[rank]):
        errs.append(f"background {bg} does not match rank {rank} {BG[rank]}")
    if exp is not None and rank != exp:
        errs.append(f"rank {rank} instead of {exp} (wrong screen or wrong order)")
    if errs:
        print(f"rank={rank} KO : " + " ; ".join(errs))
        return 1
    print(f"rank={rank} ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
