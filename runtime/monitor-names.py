#!/usr/bin/env python3
"""Prints "x,y<TAB>name" per screen, for the user32 patch (0005). x,y is the Windows position."""
import glob
import re
import subprocess


def edid_name(connector):
    for path in glob.glob(f"/sys/class/drm/card*-{connector}/edid"):
        with open(path, "rb") as f:
            edid = f.read()
        for off in (54, 72, 90, 108):
            block = edid[off:off + 18]
            if len(block) == 18 and block[:3] == b"\0\0\0" and block[3] == 0xfc:
                return block[5:].split(b"\n")[0].decode("ascii", "replace").strip()
    return ""


out = subprocess.run(["xrandr", "--listmonitors"], capture_output=True, text=True, check=True).stdout
screens = re.findall(r"^\s*\d+:\s+\+(\*?)(\S+)\s+\d+/\d+x\d+/\d+\+(-?\d+)\+(-?\d+)", out, re.M)
px, py = next(((int(x), int(y)) for star, _, x, y in screens if star), (0, 0))
for _, connector, x, y in screens:
    name = edid_name(connector)
    print(f"{int(x) - px},{int(y) - py}\t{name + ' (' + connector + ')' if name else connector}")
