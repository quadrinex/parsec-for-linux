/* Observes what the Win32 cursor APIs (the ones Parsec imports: GetCursorInfo, GetIconInfo, GetCursorPos) report under Wine. Usage: cursor_probe.exe [seconds] */
#include <stdlib.h>
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    for (int i = 0; i < secs * 4; i++) {
        POINT p = {-1, -1}; GetCursorPos(&p);
        CURSORINFO ci; ci.cbSize = sizeof ci; BOOL ok = GetCursorInfo(&ci);
        ICONINFO ii = {0}; BITMAP bm = {0}; BOOL oki = FALSE;
        if (ok && ci.hCursor) { oki = GetIconInfo(ci.hCursor, &ii); if (oki) GetObject(ii.hbmMask, sizeof bm, &bm); }
        printf("t=%d pos=%ld,%ld info_ok=%d flags=0x%lx hcur=%p pt=%ld,%ld iconinfo_ok=%d hot=%lu,%lu mask=%ldx%ld color=%p\n",
               i, p.x, p.y, ok, ci.flags, (void*)ci.hCursor, ci.ptScreenPos.x, ci.ptScreenPos.y, oki, ii.xHotspot, ii.yHotspot, bm.bmWidth, bm.bmHeight, (void*)ii.hbmColor);
        fflush(stdout); Sleep(250);
    }
    return 0;
}
