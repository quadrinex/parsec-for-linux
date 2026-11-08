/* displaylist: what user32 answers about display adapters (EnumDisplayDevicesW / EnumDisplaySettingsExW).
 * usage: displaylist [seconds]   (default: one snapshot)
 * One snapshot prints: "snap n=<adapters> | <name>@<x>,<y>:<w>x<h> ..." (CUR settings per adapter).
 * With seconds>0 it polls every 100 ms and prints each CHANGED snapshot, then "dup=<0|1> max=<n>" where dup=1 means
 * that at some instant two adapters shared a device name or a position (a placeholder shown next to the real output). */
#include "plh_test.h"
#include <wchar.h>
#define MAXA 16

static int snapshot(char *out, size_t cap, int *dup)
{
    DISPLAY_DEVICEW a; DEVMODEW m; DWORD i; int n = 0;
    WCHAR names[MAXA][32]; long px[MAXA], py[MAXA];
    size_t len = 0;
    out[0] = 0;
    for (i = 0; i < MAXA; i++) {
        memset(&a, 0, sizeof a); a.cb = sizeof a;
        if (!EnumDisplayDevicesW(NULL, i, &a, 0)) break;
        memset(&m, 0, sizeof m); m.dmSize = sizeof m;
        long x = -99999, y = -99999, w = 0, h = 0;
        if (EnumDisplaySettingsExW(a.DeviceName, ENUM_CURRENT_SETTINGS, &m, 0)) {
            x = m.dmPosition.x; y = m.dmPosition.y; w = m.dmPelsWidth; h = m.dmPelsHeight;
        }
        for (int k = 0; k < n; k++)
            if (!wcscmp(names[k], a.DeviceName) || (x != -99999 && px[k] == x && py[k] == y)) *dup = 1;
        wcsncpy(names[n], a.DeviceName, 31); names[n][31] = 0; px[n] = x; py[n] = y; n++;
        len += snprintf(out + len, cap - len, " %ls@%ld,%ld:%ldx%ld", a.DeviceName, x, y, w, h);
    }
    return n;
}

int wmain(int argc, wchar_t **argv)
{
    int secs = argc > 1 ? _wtoi(argv[1]) : 0, dup = 0, max = 0, n;
    char cur[2048], prev[2048] = "";
    double end = plh_now() + secs;
    if (secs > 0) {   /* Wine's X11 driver only follows output changes in a process that pumps messages */
        WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "dl";
        RegisterClassA(&wc);
        CreateWindowA("dl", "dl", 0, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);   /* hidden, never shown */
    }
    do {
        n = snapshot(cur, sizeof cur, &dup);
        if (n > max) max = n;
        if (strcmp(cur, prev)) { plh_out("snap n=%d |%s\n", n, cur); strcpy(prev, cur); }
        if (secs > 0) { MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg); Sleep(100); }
    } while (plh_now() < end);
    plh_out("dup=%d max=%d\n", dup, max);
    return 0;
}
