/* padprobe.exe — plays the client side of the ViGEmBus protocol with the same call sequence a
 * ViGEmClient-based program uses: enumerate the bus interface, open it, check version, plug an
 * Xbox 360 target, wait ready, wait for notifications (overlapped), submit reports, unplug.
 * Used by tests/gamepad-e2e.sh; needs no Parsec and no client. Prints one line per step. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const GUID BUS = { 0x96e42b22, 0xf5e9, 0x42f8, { 0xb0, 0x43, 0xed, 0x0f, 0x93, 0x2f, 0x01, 0x4f } };
#define CODE(fn, acc) ((0x2Au << 16) | ((acc) << 14) | ((fn) << 2))
#define IOCTL_PLUGIN  CODE(0x801, 3)
#define IOCTL_UNPLUG  CODE(0x802, 3)
#define IOCTL_VERSION CODE(0x803, 2)
#define IOCTL_READY   CODE(0x804, 3)
#define IOCTL_NOTIFY  CODE(0xA00, 3)
#define IOCTL_SUBMIT  CODE(0xA01, 3)

struct report { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };

static int ioctl_sync(HANDLE h, DWORD code, void *in, DWORD inl, const char *what)
{
    OVERLAPPED ov; DWORD n = 0;
    memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = DeviceIoControl(h, code, in, inl, NULL, 0, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &n, TRUE);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(ov.hEvent);
    printf("%s: %s (%lu)\n", what, ok ? "ok" : "FAILED", err); fflush(stdout);
    return ok;
}

int main(int argc, char **argv)
{
    int hold_ms = argc > 1 ? atoi(argv[1]) : 300;   /* time each report is held */
    HDEVINFO di = SetupDiGetClassDevsW(&BUS, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d; memset(&d, 0, sizeof d); d.cbSize = sizeof d;
    if (!SetupDiEnumDeviceInterfaces(di, NULL, &BUS, 0, &d)) { printf("enumerate: no bus interface (%lu)\n", GetLastError()); return 1; }
    DWORD need = 0; SetupDiGetDeviceInterfaceDetailW(di, &d, NULL, 0, &need, NULL);
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = calloc(1, need); det->cbSize = sizeof *det;
    if (!SetupDiGetDeviceInterfaceDetailW(di, &d, det, need, NULL, NULL)) { printf("detail: %lu\n", GetLastError()); return 1; }
    HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("open: FAILED (%lu)\n", GetLastError()); return 1; }
    printf("open: ok\n");

    DWORD ver[2] = { 8, 1 };
    if (!ioctl_sync(h, IOCTL_VERSION, ver, sizeof ver, "check version")) return 1;
    DWORD plug[4] = { 16, 1, 0, (0x045e) | (0x028eu << 16) };   /* Size, Serial, X360 wired, VID/PID */
    if (!ioctl_sync(h, IOCTL_PLUGIN, plug, sizeof plug, "plug in")) return 1;
    DWORD rdy[2] = { 8, 1 };
    ioctl_sync(h, IOCTL_READY, rdy, sizeof rdy, "wait ready");
    Sleep(1500);   /* let readers (udev ACL, evdev consumers) attach to the new device */

    /* notification request: must stay pending (no rumble), like ViGEmClient's notification thread */
    struct { DWORD size, serial; BYTE big, small, led; BYTE pad; } nin = { 12, 1, 0, 0, 0, 0 }, nout;
    OVERLAPPED nov; memset(&nov, 0, sizeof nov); nov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD nn = 0;
    BOOL nok = DeviceIoControl(h, IOCTL_NOTIFY, &nin, sizeof nin, &nout, sizeof nout, &nn, &nov);
    printf("notification: %s\n", (!nok && GetLastError() == ERROR_IO_PENDING) ? "pending" : "NOT PENDING"); fflush(stdout);

    struct report steps[] = {
        { 0x1000 | 0x0001, 255, 0, 32767, -32768, 0, 0 },      /* A + DPAD_UP, LT full, LX max, LY min */
        { 0x2000 | 0x0008 | 0x0100, 0, 128, -32768, 32767, 1000, -1000 },  /* B + DPAD_RIGHT + LB, RT half */
        { 0x4000 | 0x8000 | 0x0200 | 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0400, 0, 0, 0, 0, 32767, -32768 },
        { 0, 0, 0, 0, 0, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        struct { DWORD size, serial; struct report r; } sub = { 20, 1, steps[i] };
        char w[32]; snprintf(w, sizeof w, "report %u", i);
        if (!ioctl_sync(h, IOCTL_SUBMIT, &sub, sizeof sub, w)) return 1;
        Sleep((DWORD)hold_ms);
    }
    if (argc > 2 && !strcmp(argv[2], "leave")) { printf("leaving without unplug\n"); fflush(stdout); return 0; }   /* process exit closes the handle */
    DWORD unp[2] = { 8, 1 };
    ioctl_sync(h, IOCTL_UNPLUG, unp, sizeof unp, "unplug");
    CancelIoEx(h, &nov);
    CloseHandle(h);
    printf("done\n");
    return 0;
}
