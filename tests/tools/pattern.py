#!/usr/bin/env python3
"""Native test pattern (Qt/Wayland): one full-screen window per KWin output.

  pattern.py [--seconds N]

Each screen receives (see check_pattern.py, which shares the same description):
  - a background color specific to the screen, by its left-to-right rank
    (0 red, 1 green, 2 blue, 3 dark yellow...);
  - 4 corner squares 120x120: TL white, TR black, BL yellow, BR magenta
    (detects mirroring, rotation, offset, cropping);
  - an identity bar: 8 blocks 120x120 at y=240, x=240+120*k, bit k of
    rank+1 (white=1, black=0);
  - an animated 240x240 area at (840,720) that changes every frame (forces
    60 updates/s, ignored by the check);
  - the connector name in clear for the user.
Closes all its windows after N seconds (default 20) or on Escape.
"""
import sys

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter
from PySide6.QtWidgets import QApplication, QWidget

BG = [(220, 40, 40), (40, 200, 40), (40, 60, 220), (200, 180, 20), (20, 200, 200), (150, 60, 200)]
CORNERS = {"tl": (255, 255, 255), "tr": (0, 0, 0), "bl": (255, 255, 0), "br": (255, 0, 255)}
SQ = 120
BAR_Y, BAR_X0, NBITS = 240, 240, 8
ANIM = (840, 720, 240, 240)


class Pattern(QWidget):
    def __init__(self, screen, rank):
        super().__init__()
        self.rank, self.name, self.frame = rank, screen.name(), 0
        self.setWindowTitle(f"plh-pattern {self.name}")
        self.setWindowFlags(Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint)
        self.setScreen(screen)
        self.setGeometry(screen.geometry())
        self.showFullScreen()

    def paintEvent(self, _):
        p = QPainter(self)
        w, h = self.width(), self.height()
        p.fillRect(0, 0, w, h, QColor(*BG[self.rank % len(BG)]))
        for key, (x, y) in {"tl": (0, 0), "tr": (w - SQ, 0), "bl": (0, h - SQ), "br": (w - SQ, h - SQ)}.items():
            p.fillRect(x, y, SQ, SQ, QColor(*CORNERS[key]))
        v = self.rank + 1
        for k in range(NBITS):
            c = (255, 255, 255) if (v >> k) & 1 else (0, 0, 0)
            p.fillRect(BAR_X0 + SQ * k, BAR_Y, SQ, SQ, QColor(*c))
        g = (self.frame * 37) % 256
        p.fillRect(*ANIM, QColor(g, 255 - g, (g * 3) % 256))
        p.setPen(QColor(255, 255, 255))
        p.setFont(QFont("sans", 48, QFont.Bold))
        p.drawText(240, 520, f"plh-pattern  screen {self.rank}  {self.name}")
        p.end()
        self.frame += 1

    def keyPressEvent(self, e):
        if e.key() == Qt.Key_Escape:
            QApplication.quit()


def main():
    secs = 20.0
    if "--seconds" in sys.argv:
        secs = float(sys.argv[sys.argv.index("--seconds") + 1])
    app = QApplication(sys.argv[:1])
    screens = sorted(app.screens(), key=lambda s: (s.geometry().x(), s.geometry().y()))
    wins = [Pattern(s, i) for i, s in enumerate(screens)]
    for i, s in enumerate(screens):
        g = s.geometry()
        print(f"pattern rank={i} name={s.name()} geom={g.x()},{g.y()},{g.width()},{g.height()}", flush=True)
    t = QTimer()
    t.timeout.connect(lambda: [w.update() for w in wins])
    t.start(8)  # repaints as fast as possible; the compositor paces at vsync
    QTimer.singleShot(int(secs * 1000), app.quit)
    rc = app.exec()
    for w in wins:  # real repaints: proves the pattern animates each screen
        print(f"pattern repaints name={w.name} frames={w.frame} fps={w.frame / secs:.1f}", flush=True)
    sys.exit(rc)


if __name__ == "__main__":
    main()
