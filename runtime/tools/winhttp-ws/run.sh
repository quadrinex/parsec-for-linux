#!/usr/bin/env bash
# WinHttpWebSocket repro: the server drops the TCP connection without a close frame. Expected with wine-plh: dwError=0x2EFE (12030); plain Wine: 0x2F78.
set -euo pipefail; D=$(cd "$(dirname "$0")" && pwd); . "$D/../../env.sh"; T=/tmp/plh-eclaireur/wstest; mkdir -p $T
x86_64-w64-mingw32-gcc -o $T/cl.exe "$D/cl.c" -lwinhttp
python3 -I "$D/srv.py" close & sleep 0.5
WINEDEBUG=-all timeout 30 "$WINE" $T/cl.exe | head -3; wait
