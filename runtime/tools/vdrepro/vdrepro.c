/* Prints what user32 answers about displays: adapters, their monitors, current + all modes.
 * No client needed. Used to prove patch 0006 (virtual display placeholder). */
#include <windows.h>
#include <stdio.h>

static void modes(const WCHAR *dev)
{
    DEVMODEW m;
    int i;
    for (i = -1; i < 40; i++) {
        memset(&m, 0, sizeof(m)); m.dmSize = sizeof(m);
        if (i == 0 || i == -1) { /* -1 = ENUM_CURRENT_SETTINGS */ }
        if (!EnumDisplaySettingsExW(dev, i, &m, 0)) { if (i >= 0) break; printf("    mode[cur]: FALSE\n"); continue; }
        printf("    mode[%s%d]: %ldx%ld %luHz %lubpp pos %ld,%ld fields %#lx\n", i < 0 ? "cur" : "",
               i < 0 ? 0 : i, (long)m.dmPelsWidth, (long)m.dmPelsHeight, (unsigned long)m.dmDisplayFrequency,
               (unsigned long)m.dmBitsPerPel, (long)m.dmPosition.x, (long)m.dmPosition.y, (unsigned long)m.dmFields);
        if (i >= 3) { printf("    ...\n"); break; }
    }
}

int main(int argc, char **argv)
{
    DISPLAY_DEVICEW a, mon;
    DWORD i, j;
    (void)argc; (void)argv;
    for (i = 0;; i++) {
        memset(&a, 0, sizeof(a)); a.cb = sizeof(a);
        if (!EnumDisplayDevicesW(NULL, i, &a, 0)) { printf("adapter[%lu]: FALSE\n", (unsigned long)i); break; }
        printf("adapter[%lu]: %ls | %ls | flags %#lx\n", (unsigned long)i, a.DeviceName, a.DeviceString, (unsigned long)a.StateFlags);
        for (j = 0;; j++) {
            memset(&mon, 0, sizeof(mon)); mon.cb = sizeof(mon);
            if (!EnumDisplayDevicesW(a.DeviceName, j, &mon, 0)) break;
            printf("  monitor[%lu]: %ls | %ls | flags %#lx\n", (unsigned long)j, mon.DeviceName, mon.DeviceString, (unsigned long)mon.StateFlags);
        }
        modes(a.DeviceName);
    }
    return 0;
}
