# Notices

The project's own code is under the MIT license (`LICENSE`). These parts are not:

| Path | Origin | License |
|---|---|---|
| `runtime/patches/wine/` | Patches to Wine, derived from Wine source code | LGPL-2.1-or-later, like Wine |
| `src/encode/third_party/amf/` | AMD AMF SDK public headers | MIT, see its `LICENSE.txt` |
| `src/capture/protocols/zkde-screencast-unstable-v1.xml` | KDE Wayland protocol | LGPL-2.1-or-later |
| `src/capture/protocols/xdg-output-unstable-v1.xml` | Wayland protocols | See the file header |

## Not included

This repository contains no Parsec, Wine or DXVK binaries. `runtime/setup-prefix.sh` downloads them on the user's machine, and Parsec's own terms apply to its use.

## Trademarks

This is an independent project, not affiliated with or endorsed by Parsec or Unity Technologies. Parsec, AMD, AMF and other names are trademarks of their respective owners and are used only to describe compatibility.
