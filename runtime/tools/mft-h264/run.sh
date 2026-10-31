#!/usr/bin/env bash
# Proof: Wine's H.264 MFT correctly encodes a known red 1920x1080 in-memory NV12 frame (no green image).
set -euo pipefail; D=$(cd "$(dirname "$0")" && pwd); . "$D/../../env.sh"; T=/tmp/plh-eclaireur/mft; mkdir -p $T
x86_64-w64-mingw32-gcc -Wall -o $T/mft.exe "$D/mft.c" -lmfplat -lmfuuid -lole32 -luuid
WINEDEBUG=-all timeout 120 "$WINE" $T/mft.exe 'Z:\tmp\plh-eclaireur\mft\out.h264'
ffmpeg -v error -y -i $T/out.h264 -vf format=rgb24 -f rawvideo $T/fr.rgb
python3 -I - "$T/fr.rgb" <<'PY'
import sys
d=open(sys.argv[1],'rb').read(); W,H=1920,1080; n=len(d)//(W*H*3); o=(n-1)*W*H*3+(540*W+960)*3
px=tuple(d[o:o+3]); print("frames",n,"center pixel of last frame",px)
sys.exit(0 if n==40 and px[0]>230 and px[1]<30 and px[2]<30 else 1)
PY
echo "PASS mft-h264 (red)"
