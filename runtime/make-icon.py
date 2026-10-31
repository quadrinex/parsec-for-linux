#!/usr/bin/env python3
"""Installs the launcher icon for the application menu.

Usage: make-icon.py install|remove|check [--exe PATH] [--dest DIR]

install : reads the icon resource of the Parsec executable installed on this machine (read-only,
          standard PE resource directory, no code is read or changed) and writes it as PNG files
          under DIR/hicolor/<N>x<N>/apps/parsec-for-linux.png. If the executable or its icon is not
          there, installs a generic icon of our own (a monitor) as an SVG instead.
remove  : deletes every file `install` can have written.
check   : prints which kind of icon `install` would write ("parsec" or "generic").

Nothing from Parsec is stored in the repository: the icon is produced on the user's machine.
"""
import os
import struct
import sys
import zlib
from pathlib import Path

NAME = "parsec-for-linux"
ROOT = Path(__file__).resolve().parent.parent

GENERIC_SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <rect x="12" y="20" width="104" height="70" rx="8" fill="#2b3a4a"/>
  <rect x="19" y="27" width="90" height="56" rx="3" fill="#3daee9"/>
  <path d="M54 42 L54 68 L78 55 Z" fill="#ffffff"/>
  <rect x="56" y="90" width="16" height="14" fill="#2b3a4a"/>
  <rect x="38" y="104" width="52" height="8" rx="4" fill="#2b3a4a"/>
</svg>
"""


def data_dir():
    runtime = ROOT / "runtime"
    if os.environ.get("PLH_DATA_DIR"):
        return Path(os.environ["PLH_DATA_DIR"])
    if not os.access(runtime, os.W_OK):
        return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "parsec-for-linux"
    return runtime


def default_exe():
    user = os.environ.get("USER", "")
    for base in (data_dir() / "prefix", ROOT / "runtime/prefix"):
        p = base / "drive_c/Program Files/Parsec/parsecd.exe"
        if p.is_file():
            return p
    return data_dir() / "prefix/drive_c/Program Files/Parsec/parsecd.exe" if user else None


# ---- PE resource reading -------------------------------------------------------------------

def pe_icons(path):
    """Returns the best icon group of a PE file as a list of (width, height, bitcount, bytes)."""
    d = Path(path).read_bytes()
    if d[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec, = struct.unpack_from("<H", d, pe + 6)
    optsz, = struct.unpack_from("<H", d, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", d, opt)
    ddir = opt + (112 if magic == 0x20B else 96)
    rsrc_rva, rsrc_size = struct.unpack_from("<II", d, ddir + 16)
    if not rsrc_rva:
        raise ValueError("no resources")
    secs = []
    for i in range(nsec):
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, opt + optsz + i * 40 + 8)
        secs.append((vaddr, max(vsize, rsize), raddr))

    def off(rva):
        for va, sz, ra in secs:
            if va <= rva < va + sz:
                return ra + rva - va
        raise ValueError("bad RVA")

    base = off(rsrc_rva)

    def entries(o):
        named, ids = struct.unpack_from("<HH", d, o + 12)
        out = []
        for i in range(named + ids):
            nid, tgt = struct.unpack_from("<II", d, o + 16 + i * 8)
            out.append((nid, tgt))
        return out

    def sub(tgt):
        return base + (tgt & 0x7FFFFFFF)

    def leaf(tgt):
        """Follows name and language levels down to the data of the first entry."""
        o = sub(tgt)
        while True:
            e = entries(o)
            if not e:
                raise ValueError("empty resource directory")
            t = e[0][1]
            if t & 0x80000000:
                o = sub(t)
                continue
            rva, size = struct.unpack_from("<II", d, base + t)
            return d[off(rva):off(rva) + size]

    types = {nid: tgt for nid, tgt in entries(base) if not nid & 0x80000000}
    if 14 not in types or 3 not in types:
        raise ValueError("no icon resource")
    # RT_ICON: id -> data
    icon_dir = sub(types[3])
    icons = {}
    for nid, tgt in entries(icon_dir):
        icons[nid] = leaf(tgt)
    # first RT_GROUP_ICON
    grp = leaf(entries(sub(types[14]))[0][1])
    _, _, count = struct.unpack_from("<HHH", grp, 0)
    out = []
    for i in range(count):
        w, h, _c, _r, _pl, bits, _sz, iid = struct.unpack_from("<BBBBHHIH", grp, 6 + i * 14)
        if iid in icons:
            out.append((w or 256, h or 256, bits, icons[iid]))
    return out


def png_chunk(tag, payload):
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload))


def dib_to_png(dib):
    """32-bit BGRA DIB (as stored in .ico) to PNG bytes; None for other formats."""
    hdr, w, h2, _pl, bits, comp = struct.unpack_from("<IiiHHI", dib, 0)
    if bits != 32 or comp != 0:
        return None
    h = h2 // 2
    rows = []
    stride = w * 4
    for y in range(h - 1, -1, -1):  # bottom-up
        row = dib[hdr + y * stride: hdr + (y + 1) * stride]
        rgba = bytearray(len(row))
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = row[2::4], row[1::4], row[0::4], row[3::4]
        rows.append(b"\0" + bytes(rgba))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr)
            + png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + png_chunk(b"IEND", b""))


def pe_pngs(path):
    """Dict {size: png bytes} for the square icons we can convert, 16..256 px."""
    out = {}
    for w, h, _bits, data in pe_icons(path):
        if w != h or not 16 <= w <= 256:
            continue
        png = data if data[:8] == b"\x89PNG\r\n\x1a\n" else dib_to_png(data)
        if png:
            out[w] = png
    return out


# ---- install / remove ----------------------------------------------------------------------

def icon_files(dest):
    sizes = (16, 24, 32, 48, 64, 128, 256)
    files = [dest / "hicolor" / f"{s}x{s}" / "apps" / f"{NAME}.png" for s in sizes]
    files.append(dest / "hicolor" / "scalable" / "apps" / f"{NAME}.svg")
    return files


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("install", "remove", "check"):
        print(__doc__, file=sys.stderr)
        return 2
    cmd = args[0]
    exe = default_exe()
    dest = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "icons"
    i = 1
    while i < len(args):
        if args[i] == "--exe":
            exe = Path(args[i + 1])
            i += 2
        elif args[i] == "--dest":
            dest = Path(args[i + 1])
            i += 2
        else:
            print("unknown option:", args[i], file=sys.stderr)
            return 2

    if cmd == "remove":
        for f in icon_files(dest):
            f.unlink(missing_ok=True)
        return 0

    pngs = {}
    if exe and Path(exe).is_file():
        try:
            pngs = pe_pngs(exe)
        except (ValueError, struct.error, OSError, IndexError) as e:
            print(f"icon not read from the executable ({e}); using the generic icon", file=sys.stderr)
    if cmd == "check":
        print("parsec" if pngs else "generic")
        return 0

    for f in icon_files(dest):
        f.unlink(missing_ok=True)
    if pngs:
        for s, png in pngs.items():
            f = dest / "hicolor" / f"{s}x{s}" / "apps" / f"{NAME}.png"
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(png)
        print(f"icon: from the installed executable ({sorted(pngs)})")
    else:
        f = dest / "hicolor" / "scalable" / "apps" / f"{NAME}.svg"
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(GENERIC_SVG)
        print("icon: generic")
    return 0


if __name__ == "__main__":
    sys.exit(main())
