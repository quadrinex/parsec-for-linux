#!/usr/bin/env python3
"""Native Wayland test window (PySide6): logs to stdout, one line per event,
keys (evdev code = nativeScanCode-8), buttons, wheel (angleDelta) and global position.
Writes "READY" when it is active (focused), "INACTIVE" if it loses focus.
Closes itself after --timeout seconds. No effect outside this window."""
import sys, os
os.environ.setdefault("QT_QPA_PLATFORM", "wayland")
from PySide6.QtCore import Qt, QTimer, QEvent
from PySide6.QtWidgets import QApplication, QWidget

def out(*a):
    print(*a, flush=True)

class W(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("PLH input test window")
        self.resize(500, 400)
        self.setMouseTracking(True)
        self.setWindowFlag(Qt.WindowStaysOnTopHint, True)
    def event(self, e):
        if e.type() == QEvent.WindowActivate: out("READY")
        elif e.type() == QEvent.WindowDeactivate: out("INACTIVE")
        return super().event(e)
    def keyPressEvent(self, e):
        out("KEY down", e.nativeScanCode() - 8, "auto" if e.isAutoRepeat() else "")
    def keyReleaseEvent(self, e):
        out("KEY up", e.nativeScanCode() - 8, "auto" if e.isAutoRepeat() else "")
    def mousePressEvent(self, e):
        out("BTN down", int(e.button().value), int(e.position().x()), int(e.position().y()))
    def mouseReleaseEvent(self, e):
        out("BTN up", int(e.button().value), int(e.position().x()), int(e.position().y()))
    def wheelEvent(self, e):
        d = e.angleDelta()
        out("WHEEL", d.y(), d.x())

timeout = float(sys.argv[sys.argv.index("--timeout") + 1]) if "--timeout" in sys.argv else 15
app = QApplication(sys.argv[:1])
w = W(); w.show(); w.raise_(); w.activateWindow()
QTimer.singleShot(int(timeout * 1000), app.quit)
app.exec()
