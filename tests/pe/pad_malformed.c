/* pad_malformed.c — adversarial client of the gamepad driver (plh-vigem.sys): malformed and hostile
 * IOCTLs on the ViGEm bus handle and on the private \\.\PlhPad link. The driver must answer with an
 * error (or ignore), never crash, hang or leak a virtual pad, and a normal plug/report/unplug cycle
 * must still work afterwards. One line per case: "<name>: ok|fail <err>|pending". */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID BUS = { 0x96e42b22, 0xf5e9, 0x42f8, { 0xb0, 0x43, 0xed, 0x0f, 0x93, 0x2f, 0x01, 0x4f } };
#define CODE(fn, acc) ((0x2Au << 16) | ((acc) << 14) | ((fn) << 2))
#define IOCTL_PLUGIN  CODE(0x801, 3)
#define IOCTL_UNPLUG  CODE(0x802, 3)
#define IOCTL_VERSION CODE(0x803, 2)
#define IOCTL_SUBMIT  CODE(0xA01, 3)
#define IOCTL_PLH_PAD_GET_EVENTS 0x00222400u

/* returns 1 ok, 0 failed, -1 still pending after 300 ms (cancelled) */
static int io(HANDLE h, const char *name, DWORD code, void *in, DWORD inl, void *out, DWORD outl, int quiet)
{
    OVERLAPPED ov; DWORD n = 0; int r;
    memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = DeviceIoControl(h, code, in, inl, out, outl, &n, &ov);
    DWORD err = 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 300) == WAIT_TIMEOUT) { CancelIoEx(h, &ov); GetOverlappedResult(h, &ov, &n, TRUE); r = -1; goto done; }
        ok = GetOverlappedResult(h, &ov, &n, FALSE);
    }
    if (!ok) err = GetLastError();
    r = ok ? 1 : 0;
done:
    CloseHandle(ov.hEvent);
    if (!quiet) { if (r == 1) printf("%s: ok\n", name); else if (r == 0) printf("%s: fail %lu\n", name, err); else printf("%s: pending\n", name); fflush(stdout); }
    return r;
}

static HANDLE open_iface(const wchar_t *path)
{
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
}

int wmain(void)
{
    HDEVINFO di = SetupDiGetClassDevsW(&BUS, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA d; memset(&d, 0, sizeof d); d.cbSize = sizeof d;
    if (!SetupDiEnumDeviceInterfaces(di, NULL, &BUS, 0, &d)) { printf("enumerate: no bus interface (%lu)\n", GetLastError()); return 1; }
    DWORD need = 0; SetupDiGetDeviceInterfaceDetailW(di, &d, NULL, 0, &need, NULL);
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = calloc(1, need); det->cbSize = sizeof *det;
    if (!SetupDiGetDeviceInterfaceDetailW(di, &d, det, need, NULL, NULL)) { printf("detail: %lu\n", GetLastError()); return 1; }
    HANDLE h = open_iface(det->DevicePath);
    if (h == INVALID_HANDLE_VALUE) { printf("open: FAILED (%lu)\n", GetLastError()); return 1; }
    printf("open: ok\n");
    HANDLE priv = open_iface(L"\\\\.\\PlhPad");
    printf("open private link: %s\n", priv == INVALID_HANDLE_VALUE ? "FAILED" : "ok"); fflush(stdout);

    DWORD b[8]; BYTE out[4096];
    /* wrong sizes */
    memset(b, 0, sizeof b);
    io(h, "version_len0", IOCTL_VERSION, NULL, 0, NULL, 0, 0);
    io(h, "version_len4", IOCTL_VERSION, b, 4, NULL, 0, 0);
    io(h, "plug_len0", IOCTL_PLUGIN, NULL, 0, NULL, 0, 0);
    io(h, "plug_len4", IOCTL_PLUGIN, b, 4, NULL, 0, 0);
    b[0] = 16; b[1] = 7; b[2] = 0; b[3] = 0x045e | (0x028eu << 16);
    io(h, "plug_len8_no_type", IOCTL_PLUGIN, b, 8, NULL, 0, 0);
    b[2] = 1;                                  /* DualShock 4 target: unsupported */
    io(h, "plug_type_ds4", IOCTL_PLUGIN, b, 16, NULL, 0, 0);
    b[2] = 0xFFFFFFFFu;
    io(h, "plug_type_garbage", IOCTL_PLUGIN, b, 16, NULL, 0, 0);
    b[2] = 0; b[1] = 7;
    io(h, "plug_first", IOCTL_PLUGIN, b, 16, NULL, 0, 0);
    io(h, "plug_duplicate_serial", IOCTL_PLUGIN, b, 16, NULL, 0, 0);
    for (DWORD s = 8; s < 14; s++) { char nm[32]; b[1] = s; snprintf(nm, sizeof nm, "plug_serial_%lu", s); io(h, nm, IOCTL_PLUGIN, b, 16, NULL, 0, 0); }
    /* unplug everything we made, plus unknown / short */
    DWORD u[2] = { 8, 999 };
    io(h, "unplug_unknown_serial", IOCTL_UNPLUG, u, 8, NULL, 0, 0);
    io(h, "unplug_len4", IOCTL_UNPLUG, u, 4, NULL, 0, 0);
    io(h, "unplug_len0", IOCTL_UNPLUG, NULL, 0, NULL, 0, 0);
    for (DWORD s = 7; s < 14; s++) { u[1] = s; io(h, "unplug_cleanup", IOCTL_UNPLUG, u, 8, NULL, 0, 1); }
    /* reports */
    struct { DWORD size, serial; BYTE r[12]; } sub = { 20, 4242, { 0 } };
    io(h, "submit_unknown_serial", IOCTL_SUBMIT, &sub, 20, NULL, 0, 0);
    sub.size = 99;
    io(h, "submit_wrong_size_field", IOCTL_SUBMIT, &sub, 20, NULL, 0, 0);
    io(h, "submit_len12", IOCTL_SUBMIT, &sub, 12, NULL, 0, 0);
    io(h, "submit_len0", IOCTL_SUBMIT, NULL, 0, NULL, 0, 0);
    io(h, "submit_len4096", IOCTL_SUBMIT, out, 4096, NULL, 0, 0);
    /* wrong device type / function codes */
    io(h, "devtype_disk", 0x00070000u, b, 8, NULL, 0, 0);
    io(h, "devtype_zero", 0, NULL, 0, NULL, 0, 0);
    io(h, "code_ffffffff", 0xFFFFFFFFu, b, 8, NULL, 0, 0);
    /* private link: GET_EVENTS with odd output sizes */
    if (priv != INVALID_HANDLE_VALUE) {
        DWORD sz[] = { 0, 1, 19, 20, 21, 1000, 4096 };
        for (unsigned i = 0; i < sizeof sz / sizeof *sz; i++) { char nm[40]; snprintf(nm, sizeof nm, "get_events_out%lu", sz[i]); io(priv, nm, IOCTL_PLH_PAD_GET_EVENTS, NULL, 0, out, sz[i], 0); }
        io(priv, "get_events_with_input", IOCTL_PLH_PAD_GET_EVENTS, out, 4096, out, 20, 0);
    }
    /* fuzz: random codes of the ViGEm device type, random lengths */
    srand(12345);
    int fz[3] = { 0, 0, 0 };
    for (int i = 0; i < 300; i++) {
        DWORD code = (0x2Au << 16) | ((DWORD)(rand() % 4) << 14) | ((DWORD)(rand() % 0xC00 + (rand() % 2 ? 0x800 : 0)) << 2) | (rand() % 4);
        DWORD inl = (DWORD)(rand() % 40), outl = (DWORD)(rand() % 40);
        for (int k = 0; k < 8; k++) b[k] = (DWORD)rand();
        BYTE ib[64]; for (int k = 0; k < 64; k++) ib[k] = (BYTE)rand();
        int r = io(h, "fuzz", code, ib, inl, out, outl, 1);
        fz[r + 1]++;
    }
    printf("fuzz: 300 requests, ok=%d fail=%d pending=%d\n", fz[2], fz[1], fz[0]); fflush(stdout);
    /* the driver must still work: normal cycle */
    DWORD ver[2] = { 8, 1 };
    int a = io(h, "final_version", IOCTL_VERSION, ver, 8, NULL, 0, 0);
    DWORD plug[4] = { 16, 77, 0, 0x045e | (0x028eu << 16) };
    int p = io(h, "final_plug", IOCTL_PLUGIN, plug, 16, NULL, 0, 0);
    Sleep(1500);
    struct { DWORD size, serial; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } rep = { 20, 77, 0x1000, 0, 0, 0, 0, 0, 0 };
    int s = io(h, "final_report", IOCTL_SUBMIT, &rep, 20, NULL, 0, 0);
    Sleep(600);
    u[1] = 77;
    int q = io(h, "final_unplug", IOCTL_UNPLUG, u, 8, NULL, 0, 0);
    printf("final cycle: %s\n", a == 1 && p == 1 && s == 1 && q == 1 ? "ok" : "BROKEN");
    return 0;
}
