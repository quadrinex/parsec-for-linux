#!/usr/bin/env bash
# Menu entry, icon, settings file and tray code: everything checkable without a Parsec session.
# Works in a temporary HOME-like tree; nothing is written outside it and no host is touched.
. "$(dirname "$0")/lib/common.sh"
ROOT=$(cd "$TESTS_DIR/.." && pwd)
need_cmd python3 desktop-file-validate
T=$(mktemp -d /tmp/plh-t07.XXXXXX); trap 'rm -rf "$T"' EXIT
export XDG_DATA_HOME=$T/data XDG_CACHE_HOME=$T/cache XDG_CONFIG_HOME=$T/config PLH_DATA_DIR=$T/plh
mkdir -p "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME"
HOST=$ROOT/bin/parsec-linux-host

# 1. the template and the generated menu entry are valid, and Exec points at an executable
desktop-file-validate "$ROOT/packaging/desktop/parsec-for-linux.desktop" 2>&1 | grep -v "@BIN@" | grep . && fail "template invalid"
"$HOST" launcher add 2>"$T/err" || fail "launcher add failed: $(cat "$T/err")"
D=$XDG_DATA_HOME/applications/parsec-for-linux.desktop
[ -f "$D" ] || fail "no .desktop written"
desktop-file-validate "$D" || fail "generated .desktop invalid"
grep -q '^Name=Parsec$' "$D" || fail "entry not named Parsec"
exe=$(sed -n 's/^Exec=\(.*\) open$/\1/p' "$D")
[ -x "$exe" ] || fail "Exec target not executable: $exe"
grep -q '@BIN@' "$D" && fail "placeholder left in .desktop"
echo "  # menu entry ok"

# 2. an icon exists for the entry (the installed Parsec's, else the generic one)
icons=$(find "$XDG_DATA_HOME/icons" -name 'parsec-for-linux.*' | wc -l)
[ "$icons" -ge 1 ] || fail "no icon installed"
echo "  # icon files: $icons"

# 3. remove undoes everything, and a second add/remove is harmless
"$HOST" launcher remove || fail "launcher remove failed"
left=$(find "$XDG_DATA_HOME" -type f | head)
[ -z "$left" ] || fail "files left after remove: $left"
"$HOST" launcher remove || fail "second remove failed"

# 4. uninstall.sh --dry-run lists the launcher removal and changes nothing
"$HOST" launcher add 2>/dev/null
out=$("$ROOT/uninstall.sh" --dry-run --keep-prefix 2>&1) || fail "uninstall --dry-run failed"
echo "$out" | grep -q 'launcher remove' || fail "uninstall.sh does not remove the launcher"
[ -f "$D" ] || fail "--dry-run removed the menu entry"
"$HOST" launcher remove

# 5. the settings file round trip, and env.sh reads it (and an environment variable still wins)
python3 -I - "$ROOT" "$T/plh/config.env" <<'PY' || fail "config round trip"
import sys
sys.path.insert(0, sys.argv[1] + "/runtime")
import plhconfig as c
p = sys.argv[2]
assert c.read(p) == {}
c.write({"PLH_ENABLE_AMF": "0", "PLH_ENABLE_INPUT": "1"}, p)
open(p, "a").write("# my comment\nexport OTHER=keep\n")
c.write({"PLH_ENABLE_AMF": "1", "PLH_ENABLE_CLIPBOARD": "0"}, p)
v = c.read(p)
assert v == {"PLH_ENABLE_AMF": "1", "PLH_ENABLE_INPUT": "1", "PLH_ENABLE_CLIPBOARD": "0"}, v
t = open(p).read()
assert "# my comment" in t and "export OTHER=keep" in t
try:
    c.write({"PLH_ENABLE_AMF": '1"; rm -rf ~; "'}, p)
    raise SystemExit("unsafe value accepted")
except ValueError:
    pass
PY
[ "$(bash -c '. "$1/runtime/env.sh"; echo "$PLH_ENABLE_CLIPBOARD"' _ "$ROOT")" = 0 ] || fail "env.sh did not read the saved setting"
[ "$(PLH_ENABLE_CLIPBOARD=1 bash -c '. "$1/runtime/env.sh"; echo "$PLH_ENABLE_CLIPBOARD"' _ "$ROOT")" = 1 ] || fail "environment does not override the file"
echo "  # config round trip ok"

# 6. the tray module loads and its logic works (offscreen, no tray needed)
python3 -c 'import PySide6.QtWidgets' 2>/dev/null || skip "PySide6 missing: config, launcher and uninstall checks passed, tray code not checked"
QT_QPA_PLATFORM=offscreen python3 -I - "$ROOT" <<'PY' || fail "tray module"
import sys, importlib.machinery, importlib.util
root = sys.argv[1]
l = importlib.machinery.SourceFileLoader("ptray", root + "/bin/parsec-tray")
spec = importlib.util.spec_from_loader("ptray", l)
m = importlib.util.module_from_spec(spec)
l.exec_module(m)
st = "parsecd running\nplh-bridge running\nplh-input-hook running\nplh-pad-relay running\nplh-clipd running\nplh-clipsync running\nlayout-watch running\nclient connected\n"
comp, cl = m.parse_status(st)
on = {"PLH_ENABLE_INPUT": "1", "PLH_ENABLE_GAMEPAD": "1", "PLH_ENABLE_CLIPBOARD": "1"}
assert m.overall(comp, cl, on) == "connected"
assert m.overall(comp, False, on) == "ready"
comp["plh-clipd"] = False
assert m.overall(comp, False, on) == "degraded"
assert m.overall(comp, False, dict(on, PLH_ENABLE_CLIPBOARD="0")) == "ready"   # disabled feature is not a fault
comp["parsecd"] = False
assert m.overall(comp, False, on) == "stopped"
assert m.overall({}, False, on) == "unknown"
app = m.QApplication([])
assert not m.status_icon("connected").isNull()
class Stub:
    state, components = "ready", {"parsecd": True}
w = m.Settings(Stub())
assert set(w.values()) == set(m.plhconfig.OPTIONS) and set(w.values().values()) <= {"0", "1"}
PY
pass "launcher add/remove (+icon), uninstall --dry-run, config round trip, env.sh, tray logic"
