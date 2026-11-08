/* Prints the monitors Wine reports (EnumDisplayMonitors + EnumDisplayDevicesA) and logs
 * WM_DISPLAYCHANGE, to see whether Wine follows output hotplug live (S5). Observation only.
 * Usage: displaywatch.exe <seconds>  (polls every 250 ms, prints on every change) */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last[4096];
static DWORD t0;
static int ndisp;

static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    char *p = (char *)lp; MONITORINFOEXA mi = {.cbSize = sizeof(mi)};
    (void)dc; (void)r;
    if (GetMonitorInfoA(m, (MONITORINFO *)&mi))
        sprintf(p + strlen(p), " [%s %ld,%ld %ldx%ld%s]", mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                (mi.dwFlags & MONITORINFOF_PRIMARY) ? " P" : "");
    return TRUE;
}

static void snapshot(const char *why)
{
    char cur[4096] = ""; DISPLAY_DEVICEA dd = {.cb = sizeof(dd)}; int n = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)cur);
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) n++;
    sprintf(cur + strlen(cur), " devices=%d vscreen=%d,%d %dx%d", n, GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (strcmp(cur, last) || !strcmp(why, "start")) {
        printf("t=%5lums %-8s%s\n", (unsigned long)(GetTickCount() - t0), why, cur); fflush(stdout);
        strcpy(last, cur);
    }
}

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DISPLAYCHANGE) {
        ndisp++;
        printf("t=%5lums WM_DISPLAYCHANGE #%d bpp=%u %ux%u\n", (unsigned long)(GetTickCount() - t0), ndisp,
               (unsigned)w, (unsigned)LOWORD(l), (unsigned)HIWORD(l));
        fflush(stdout);
    }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20; WNDCLASSA wc = {.lpfnWndProc = wp, .lpszClassName = "dw", .hInstance = GetModuleHandleA(NULL)};
    RegisterClassA(&wc);
    CreateWindowA("dw", "dw", 0, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);   /* hidden top-level: gets broadcasts */
    t0 = GetTickCount(); snapshot("start");
    while ((int)(GetTickCount() - t0) < secs * 1000) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        snapshot("change");
        Sleep(250);
    }
    printf("done, WM_DISPLAYCHANGE count=%d\n", ndisp);
    return 0;
}
