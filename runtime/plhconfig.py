"""Settings file of the Parsec host (config.env), shared by bin/parsec-tray and the tests.

The file is sourced by runtime/env.sh. Each setting is a pair of lines
    : "${PLH_ENABLE_AMF:=1}"
    export PLH_ENABLE_AMF
so an environment variable set by the caller still wins. This module reads and rewrites only the
keys it is asked to change and keeps every other line (comments, other variables) as it is.

No Qt here: it must import without a display.
"""
import os
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / "runtime"

# key -> (label, default). Defaults are those of runtime/run-parsec.sh when the key is absent.
OPTIONS = {
    "PLH_ENABLE_AMF": ("Hardware video encoding (AMF API over VA-API)", "1"),
    "PLH_ENABLE_INPUT": ("Remote keyboard and mouse", "1"),
    "PLH_ENABLE_GAMEPAD": ("Remote gamepads", "1"),
    "PLH_ENABLE_CLIPBOARD": ("Text clipboard sharing", "1"),
    "PLH_BRIDGE_VERBOSE": ("Verbose bridge log (for bug reports)", "0"),
    "PLH_VIRTUAL_DISPLAY": ("Offer a virtual display", "0"),
    "PLH_VIRTUAL_DISPLAY_AUTO": ("Use a virtual display when the screens are off", "0"),
    "PLH_VIRTUAL_DISPLAY_MODE": ("Virtual display resolution", "1920x1080"),
}

# Options that are not on/off switches: key -> allowed values (first = default). A settings window must offer
# these as a choice, not as a checkbox; see valid_mode() for the accepted free-form syntax.
CHOICES = {
    "PLH_VIRTUAL_DISPLAY_MODE": ["1920x1080", "2560x1440", "1280x720", "3840x2160"],
}


def valid_mode(text):
    """True for 'WIDTHxHEIGHT' with even sizes within the encoder limits (320..4096 wide, 200..2304 high)."""
    m = re.fullmatch(r"(\d{3,4})x(\d{3,4})", str(text))
    if not m:
        return False
    w, h = int(m.group(1)), int(m.group(2))
    return 320 <= w <= 4096 and 200 <= h <= 2304 and w % 2 == 0 and h % 2 == 0

_LINE = re.compile(r'^:\s+"\$\{(\w+):=(.*)\}"\s*$')
_HEADER = (
    "# shellcheck shell=bash\n"
    "# Written by the settings window (bin/parsec-linux-host config) — bridge configuration.\n"
    '# Uses ": \\"${VAR:=val}\\"" so an environment variable overrides the file.\n\n'
)


def data_dir():
    """Same rule as runtime/env.sh."""
    if os.environ.get("PLH_DATA_DIR"):
        return Path(os.environ["PLH_DATA_DIR"])
    if not os.access(RUNTIME, os.W_OK):
        return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "parsec-for-linux"
    return RUNTIME


def config_path():
    """Where settings are saved. env.sh reads this file first, then runtime/config.env."""
    d = data_dir()
    return d / "config.env"


def traces_dir():
    return data_dir() / "traces"


def read(path=None):
    """Returns {key: value} for every `: "${KEY:=value}"` line of the file ({} if missing)."""
    path = Path(path or config_path())
    out = {}
    try:
        text = path.read_text()
    except OSError:
        return out
    for line in text.splitlines():
        m = _LINE.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def effective(path=None):
    """Value of each known option: the saved one, else the value in runtime/config.env, else the default."""
    vals = {k: d for k, (_l, d) in OPTIONS.items()}
    for p in (RUNTIME / "config.env", Path(path or config_path())):
        vals.update({k: v for k, v in read(p).items() if k in OPTIONS})
    return vals


def write(updates, path=None):
    """Sets the given keys in the file, creating it if needed. Atomic. Returns the path."""
    path = Path(path or config_path())
    for k, v in updates.items():
        if not re.fullmatch(r"\w+", k) or re.search(r'[\n"}$`\\]', str(v)):
            raise ValueError(f"unsafe setting {k!r}")
        if k in CHOICES and not valid_mode(v):
            raise ValueError(f"invalid value for {k}: {v!r}")
    try:
        lines = path.read_text().splitlines()
    except OSError:
        lines = _HEADER.splitlines()
    todo = dict(updates)
    for i, line in enumerate(lines):
        m = _LINE.match(line)
        if m and m.group(1) in todo:
            lines[i] = f': "${{{m.group(1)}:={todo.pop(m.group(1))}}}"'
    for k, v in todo.items():
        lines += [f': "${{{k}:={v}}}"', f"export {k}"]
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".config.env.")
    try:
        with os.fdopen(fd, "w") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise
    return path
