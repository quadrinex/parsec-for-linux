/* plh-pad-relay.exe — PE, dedicated prefix. Installs/starts plh-vigem.sys, publishes the ViGEmBus
 * device interface (volatile registry keys, redone at every wineserver start), then relays the
 * gamepad events queued by the driver to plh-bridge (input service, PLH_MSG_INPUT_GAMEPAD).
 *
 * Usage: plh-pad-relay.exe [--install PATH_TO_SYS] [--setup-only] [--uninstall] [--seconds N] [-v]
 * --uninstall stops and deletes the driver service and its file, then exits (prefix left clean).
 * Prints READY once the interface is published. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../bridge/client/plh_client.h"
#include "plh_pad_drv.h"

#define SVC_NAME L"PlhVigem"
#define CLS L"System\\CurrentControlSet\\Control\\DeviceClasses\\{96E42B22-F5E9-42F8-B043-ED0F932F014F}"
#define IFK L"##?#ROOT#PLHVIGEM#0001#{96E42B22-F5E9-42F8-B043-ED0F932F014F}"
#define INST L"ROOT\\PLHVIGEM\\0001"
#define SYM  L"\\\\?\\ROOT#PLHVIGEM#0001#{96E42B22-F5E9-42F8-B043-ED0F932F014F}"

static int verbose;

static int put_sz(HKEY k, const WCHAR *name, const WCHAR *v)
{ return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(WCHAR))) == 0; }
static int put_dw(HKEY k, const WCHAR *name, DWORD v)
{ return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4) == 0; }

static HKEY vkey(const WCHAR *path)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &k, NULL)) return NULL;
    return k;
}

/* What Wine's setupapi needs to list a device interface (derived from the keys Wine itself creates). */
static int publish_interface(void)
{
    HKEY k;
    if (!(k = vkey(L"System\\CurrentControlSet\\Enum\\ROOT\\PLHVIGEM\\0001"))) return 0;
    put_dw(k, L"Phantom", 0);
    put_sz(k, L"ClassGUID", L"{4d36e97d-e325-11ce-bfc1-08002be10318}");   /* "System" class */
    RegCloseKey(k);
    if (!(k = vkey(CLS L"\\" IFK))) return 0;
    put_sz(k, L"DeviceInstance", INST); RegCloseKey(k);
    if (!(k = vkey(CLS L"\\" IFK L"\\#"))) return 0;
    put_sz(k, L"SymbolicLink", SYM); RegCloseKey(k);
    if (!(k = vkey(CLS L"\\" IFK L"\\#\\Control"))) return 0;
    put_dw(k, L"Linked", 1); RegCloseKey(k);
    return 1;
}

static int install_driver(const char *src)
{
    WCHAR dst[MAX_PATH], bin[MAX_PATH];
    GetSystemDirectoryW(dst, MAX_PATH);
    wcscat(dst, L"\\drivers");
    CreateDirectoryW(dst, NULL);
    wcscat(dst, L"\\plh-vigem.sys");
    if (src) {
        WCHAR wsrc[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, src, -1, wsrc, MAX_PATH);
        /* the service may hold the file; copy over it only when stopped (below) */
        SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (!m) return 0;
        SC_HANDLE s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
        SERVICE_STATUS ss;
        if (s) { ControlService(s, SERVICE_CONTROL_STOP, &ss); Sleep(300); CloseServiceHandle(s); }
        if (!CopyFileW(wsrc, dst, FALSE)) { fprintf(stderr, "copy driver: %lu\n", GetLastError()); CloseServiceHandle(m); return 0; }
        CloseServiceHandle(m);
    }
    wcscpy(bin, L"\\??\\"); wcscat(bin, dst);
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!m) return 0;
    SC_HANDLE s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!s)
        s = CreateServiceW(m, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
                           SERVICE_ERROR_NORMAL, dst, NULL, NULL, NULL, NULL, NULL);
    (void)bin;
    int ok = 0;
    if (s) {   /* start through a fresh handle: starting on the CreateService handle fails (1055) in a fresh prefix */
        CloseServiceHandle(s);
        Sleep(500);
        s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    }
    if (s) {
        /* in a fresh prefix services.exe keeps its database locked (1055) for ~40 s after the first start */
        for (int t = 0; t < 60 && !ok; t++) {
            SERVICE_STATUS ss;
            ok = StartServiceW(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING ||
                 (QueryServiceStatus(s, &ss) && ss.dwCurrentState == SERVICE_RUNNING);
            if (!ok) {
                DWORD e = GetLastError();
                if (e != ERROR_SERVICE_DATABASE_LOCKED && e != ERROR_SERVICE_REQUEST_TIMEOUT) { fprintf(stderr, "StartService: %lu\n", e); break; }
                if (t == 0) fprintf(stderr, "StartService: %lu (service database busy, waiting)\n", e);
                Sleep(2000);
            }
        }
        CloseServiceHandle(s);
    } else fprintf(stderr, "CreateService: %lu\n", GetLastError());
    CloseServiceHandle(m);
    return ok;
}

static int uninstall_driver(void)
{
    WCHAR dst[MAX_PATH];
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!m) return 0;
    SC_HANDLE s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (s) {
        SERVICE_STATUS ss;
        ControlService(s, SERVICE_CONTROL_STOP, &ss);
        Sleep(300);
        DeleteService(s);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(m);
    GetSystemDirectoryW(dst, MAX_PATH);
    wcscat(dst, L"\\drivers\\plh-vigem.sys");
    DeleteFileW(dst);
    return 1;
}

int main(int argc, char **argv)
{
    const char *sys = NULL;
    int secs = 0, setup_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--install") && i + 1 < argc) sys = argv[++i];
        else if (!strcmp(argv[i], "--setup-only")) setup_only = 1;
        else if (!strcmp(argv[i], "--uninstall")) return uninstall_driver() ? 0 : 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "usage: plh-pad-relay.exe [--install PATH_TO_SYS] [--setup-only] [--uninstall] [--seconds N] [-v]\n"); return 2; }
    }
    if (!install_driver(sys)) return 1;
    if (!publish_interface()) { fprintf(stderr, "cannot publish the device interface\n"); return 1; }
    HANDLE dev = CreateFileW(L"\\\\.\\PlhPad", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) { fprintf(stderr, "open \\\\.\\PlhPad: %lu\n", GetLastError()); return 1; }
    printf("READY\n"); fflush(stdout);
    if (setup_only) return 0;

    struct plh_conn *conn = NULL;
    struct plh_hello_reply hr;
    int st = PLH_E_IO;
    for (int t = 0; t < 100 && st != PLH_OK; t++) {
        st = plh_conn_open(&conn, PLH_CLIENT_INPUT, "plh-pad-relay", &hr);
        if (st != PLH_OK) Sleep(200);
    }
    if (st != PLH_OK) { fprintf(stderr, "plh-pad-relay: bridge unavailable (%d)\n", st); return 1; }
    if (!(hr.services & (1u << PLH_SVC_INPUT))) { fprintf(stderr, "plh-pad-relay: input service missing from the bridge\n"); return 1; }

    struct plh_input_gamepad state[PLH_PAD_SLOTS];
    memset(state, 0, sizeof state);
    DWORD end = secs > 0 ? GetTickCount() + (DWORD)secs * 1000 : 0;
    for (;;) {
        struct plh_pad_evt ev[32];
        DWORD n = 0;
        if (!DeviceIoControl(dev, IOCTL_PLH_PAD_GET_EVENTS, NULL, 0, ev, sizeof ev, &n, NULL)) { fprintf(stderr, "GET_EVENTS: %lu\n", GetLastError()); break; }
        n /= sizeof ev[0];
        for (DWORD i = 0; i < n; i++) {
            struct plh_input_gamepad *g = &state[ev[i].slot % PLH_PAD_SLOTS];
            g->index = ev[i].slot;
            if (ev[i].type == PLH_PAD_EVT_PLUG) { memset(g, 0, sizeof *g); g->index = ev[i].slot; g->connected = 1; }
            else if (ev[i].type == PLH_PAD_EVT_UNPLUG) { memset(g, 0, sizeof *g); g->index = ev[i].slot; }
            else {
                g->connected = 1; g->buttons = ev[i].buttons; g->left_trigger = ev[i].lt; g->right_trigger = ev[i].rt;
                g->lx = ev[i].lx; g->ly = ev[i].ly; g->rx = ev[i].rx; g->ry = ev[i].ry;
            }
            if (verbose) { printf("slot %u type %u connected %u buttons %04x\n", g->index, ev[i].type, g->connected, g->buttons); fflush(stdout); }
            if (plh_conn_send(conn, PLH_MSG_INPUT_GAMEPAD, g, sizeof *g) != PLH_OK) { fprintf(stderr, "bridge gone\n"); goto out; }
        }
        if (end && (LONG)(GetTickCount() - end) >= 0) break;
        if (!n) Sleep(4);
    }
out:
    plh_conn_close(conn);   /* the bridge releases this client's virtual pads */
    return 0;
}
