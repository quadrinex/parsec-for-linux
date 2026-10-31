#!/usr/bin/env bash
# Installs/removes amfrt64.dll (N3) in the DEDICATED Wine prefix only (system32).
#   install-amf.sh          : copie build/encode/amf/amfrt64.dll
#   install-amf.sh --undo   : removes amfrt64.dll from the prefix
# Idempotent. Never touches the system Wine or ~/.wine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
. "$ROOT/runtime/env.sh"
S="$WINEPREFIX/drive_c/windows/system32"
P="$ROOT/build/encode/amf/amfrt64.dll"
[ -d "$S" ] || { echo "dedicated prefix missing" >&2; exit 1; }
if [ "${1:-}" = --undo ]; then rm -f "$S/amfrt64.dll"; echo "amfrt64.dll removed"; exit 0; fi
[ -f "$P" ] || { echo "not built: make -C src/encode amf" >&2; exit 1; }
cp -f "$P" "$S/amfrt64.dll"; echo "amfrt64.dll installed in $S"
