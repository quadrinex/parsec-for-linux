/* Minimal PE test: the mingw chain produces an .exe that runs under Wine in
 * the dedicated prefix; prints the Wine version and the screen layout
 * as Windows (hence Parsec) sees it: EnumDisplayMonitors + virtual screen. */
#include "plh_test.h"

static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r; (void)lp;
    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(m, (MONITORINFO *)&mi);
    plh_out("monitor name=%ls rect=%ld,%ld,%ld,%ld primary=%d\n", mi.szDevice,
            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0);
    return TRUE;
}

int wmain(void) {
    const char *wv = plh_wine_version();
    plh_out("wine_version=%s\n", wv ? wv : "(not Wine)");
    OK(wv != NULL, "runs under Wine");
    int n = GetSystemMetrics(SM_CMONITORS);
    plh_out("monitors=%d\n", n);
    plh_out("virtual_screen=%d,%d,%d,%d\n", GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN));
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
    OK(n >= 1, "at least one screen seen by Windows");
    return plh_failures ? 1 : 0;
}
