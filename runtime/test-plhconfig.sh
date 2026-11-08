#!/bin/bash
# valid_mode() (even sizes, <=4096x2304) and write() rejecting invalid CHOICES values. Run: runtime/test-plhconfig.sh
set -eu
cd "$(dirname "$0")"
exec python3 -I - <<'PY'
import sys, tempfile, os
sys.path.insert(0, ".")
import plhconfig as c
for v, ok in [("1920x1080", 1), ("3840x2160", 1), ("4096x2304", 1), ("1921x1080", 0), ("1920x1081", 0),
              ("4096x2400", 0), ("4098x1080", 0), ("abc", 0), ("9999x9999", 0)]:
    assert c.valid_mode(v) == bool(ok), v
    p = os.path.join(tempfile.mkdtemp(), "cfg")
    try:
        c.write({"PLH_VIRTUAL_DISPLAY_MODE": v}, p); r = 1
    except ValueError:
        r = 0
    assert r == ok, v
print("PASS: plhconfig valid_mode/write")
PY
