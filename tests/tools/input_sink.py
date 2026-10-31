#!/usr/bin/env python3
"""Native input receiver (Qt/Wayland): one gray full-screen window per
KWin output, which logs what it really receives from the compositor.

  input_sink.py [--seconds N]

Lines emitted (stdout):
  ready <connector> <x>,<y>,<w>,<h>
  press <connector> local=<x>,<y> global=<gx>,<gy> button=<n>
  key press|release key=<Qt code> native=<scancode> text=<text>
  wheel <connector> delta=<dy>
Global = KWin screen position + local position (Wayland gives no global
coordinates to clients). The cursor stays visible; nothing is
passed on to other applications during the test (windows in the foreground).
"""
import sys

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtWidgets import QApplication, QWidget


def out(s):
    print(s, flush=True)


class Sink(QWidget):
    def __init__(self, screen):
        super().__init__()
        self.scr = screen
        self.setWindowTitle(f"plh-input-sink {screen.name()}")
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self.setScreen(screen)
        self.setGeometry(screen.geometry())
        self.setMouseTracking(True)
        self.showFullScreen()

    def paintEvent(self, _):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(60, 60, 60))
        p.setPen(QColor(255, 255, 255))
        p.setFont(QFont("sans", 32))
        p.drawText(100, 100, f"plh input test ({self.scr.name()}) — touch nothing, Escape to quit")
        p.end()

    def focusInEvent(self, e):
        out(f"focus in {self.scr.name()}")

    def focusOutEvent(self, e):
        out(f"focus out {self.scr.name()}")

    def mousePressEvent(self, e):
        g = self.scr.geometry()
        lx, ly = round(e.position().x()), round(e.position().y())
        out(f"press {self.scr.name()} local={lx},{ly} global={g.x() + lx},{g.y() + ly} button={e.button().value}")

    def wheelEvent(self, e):
        out(f"wheel {self.scr.name()} delta={e.angleDelta().y()}")

    def keyPressEvent(self, e):
        if e.key() == Qt.Key_Escape:
            QApplication.quit()
        out(f"key press key={e.key()} native={e.nativeScanCode()} text={e.text()!r}")

    def keyReleaseEvent(self, e):
        out(f"key release key={e.key()} native={e.nativeScanCode()} text={e.text()!r}")


def main():
    secs = float(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 20.0
    app = QApplication(sys.argv[:1])
    wins = [Sink(s) for s in app.screens()]
    for s in app.screens():
        g = s.geometry()
        out(f"ready {s.name()} {g.x()},{g.y()},{g.width()},{g.height()}")
    wins[0].activateWindow()
    QTimer.singleShot(int(secs * 1000), app.quit)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
