/* plh-input-hook.exe — relay of input injected by Parsec (PE, dedicated prefix).
 *
 * Under Wine, SendInput / mouse_event / keybd_event (public APIs a Parsec host uses) only
 * change wineserver's input state: nothing reaches the Wayland compositor. This program
 * installs PUBLIC low-level hooks (SetWindowsHookEx WH_MOUSE_LL / WH_KEYBOARD_LL), which see
 * in the Wine desktop all input flagged INJECTED (LLMHF_INJECTED / LLKHF_INJECTED),
 * swallow it (no double delivery: the real input comes back through KWin -> XWayland -> Wine), and
 * send it to the plh-bridge daemon (service 0x03), which replays it on the real desktop.
 * No Parsec binary is modified or instrumented: observation through public Windows APIs.
 *
 * Motion: the hook only receives the final position (pt). Since events are swallowed,
 * the Wine cursor c stays fixed; we send MOVE_ABS(pt) AND an inferred MOVE_REL (pt - c, flag
 * PLH_IEV_F_INFERRED); the daemon chooses (absolute by default). Wine's acceleration of relative
 * motion (MouseSpeed/Threshold) is disabled for the session (SPI_SETMOUSE, not persistent).
 *
 * Usage: plh-input-hook.exe [--dry-run] [--passthrough] [--seconds N] [-v]
 *   --dry-run     does not open the bridge, prints the events (stdout)
 *   --passthrough does not swallow events (diagnostics)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../bridge/client/plh_client.h"

static HHOOK hm, hk;
static struct plh_conn *conn;
static int dry, passthrough, verbose;
static volatile LONG sent, failed;

static void ship(const struct plh_input_ev *ev, int n)
{
    if (verbose || dry) {
        for (int i = 0; i < n; i++)
            printf("EV type=%u flags=0x%x code=0x%x a=%d b=%d\n", ev[i].type, ev[i].flags, ev[i].code, ev[i].a, ev[i].b);
        fflush(stdout);
    }
    if (dry || !conn) return;
    struct { struct plh_input_events_hdr h; struct plh_input_ev e[2]; } m;
    m.h.count = (uint32_t)n; m.h.reserved = 0;
    memcpy(m.e, ev, sizeof(ev[0]) * (size_t)n);
    if (plh_conn_send(conn, PLH_MSG_INPUT_EVENTS, &m, (uint32_t)(sizeof m.h + sizeof(ev[0]) * (size_t)n)) == PLH_OK)
        InterlockedIncrement(&sent);
    else
        InterlockedIncrement(&failed);
}

static LRESULT CALLBACK mouse_ll(int code, WPARAM wp, LPARAM lp)
{
    if (code != HC_ACTION) return CallNextHookEx(hm, code, wp, lp);
    const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
    if (!(m->flags & LLMHF_INJECTED)) return CallNextHookEx(hm, code, wp, lp);
    struct plh_input_ev ev[2];
    memset(ev, 0, sizeof ev);
    int n = 1;
    ev[0].time_ms = m->time;
    switch (wp) {
    case WM_MOUSEMOVE: {
        POINT c; GetCursorPos(&c);
        ev[0].type = PLH_IEV_MOVE_ABS; ev[0].a = m->pt.x; ev[0].b = m->pt.y;
        ev[1].type = PLH_IEV_MOVE_REL; ev[1].flags = PLH_IEV_F_INFERRED;
        ev[1].a = m->pt.x - c.x; ev[1].b = m->pt.y - c.y; ev[1].time_ms = m->time;
        n = 2;
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        ev[0].type = PLH_IEV_BUTTON; ev[0].code = PLH_IBTN_LEFT; ev[0].b = wp == WM_LBUTTONDOWN; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        ev[0].type = PLH_IEV_BUTTON; ev[0].code = PLH_IBTN_RIGHT; ev[0].b = wp == WM_RBUTTONDOWN; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        ev[0].type = PLH_IEV_BUTTON; ev[0].code = PLH_IBTN_MIDDLE; ev[0].b = wp == WM_MBUTTONDOWN; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        ev[0].type = PLH_IEV_BUTTON;
        ev[0].code = HIWORD(m->mouseData) == XBUTTON2 ? PLH_IBTN_X2 : PLH_IBTN_X1;
        ev[0].b = wp == WM_XBUTTONDOWN; break;
    case WM_MOUSEWHEEL:
        ev[0].type = PLH_IEV_WHEEL; ev[0].a = (short)HIWORD(m->mouseData); break;
    case WM_MOUSEHWHEEL:
        ev[0].type = PLH_IEV_WHEEL; ev[0].b = (short)HIWORD(m->mouseData); break;
    default:
        return CallNextHookEx(hm, code, wp, lp);
    }
    ship(ev, n);
    return passthrough ? CallNextHookEx(hm, code, wp, lp) : 1;
}

static LRESULT CALLBACK kbd_ll(int code, WPARAM wp, LPARAM lp)
{
    if (code != HC_ACTION) return CallNextHookEx(hk, code, wp, lp);
    const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
    if (!(k->flags & LLKHF_INJECTED)) return CallNextHookEx(hk, code, wp, lp);
    struct plh_input_ev ev;
    memset(&ev, 0, sizeof ev);
    ev.type = PLH_IEV_KEY;
    ev.code = (uint16_t)k->scanCode;      /* 0 if the caller only gave the VK */
    ev.a = (int32_t)k->vkCode;
    ev.b = !(k->flags & LLKHF_UP);
    ev.flags = (k->flags & LLKHF_EXTENDED) ? PLH_IEV_F_EXTENDED : 0;
    ev.time_ms = k->time;
    ship(&ev, 1);
    return passthrough ? CallNextHookEx(hk, code, wp, lp) : 1;
}

struct mons { struct plh_input_layout_hdr h; struct plh_input_monitor m[16]; };
static BOOL CALLBACK mon_cb(HMONITOR hmon, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r;
    struct mons *ms = (struct mons *)lp;
    MONITORINFO mi; mi.cbSize = sizeof mi;
    if (ms->h.count < 16 && GetMonitorInfoW(hmon, &mi)) {
        struct plh_input_monitor *m = &ms->m[ms->h.count++];
        m->x = mi.rcMonitor.left; m->y = mi.rcMonitor.top;
        m->w = mi.rcMonitor.right - mi.rcMonitor.left; m->h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0; m->reserved = 0;
    }
    return TRUE;
}

static void send_layout(void)
{
    struct mons ms;
    memset(&ms, 0, sizeof ms);
    EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)&ms);
    for (uint32_t i = 0; i < ms.h.count; i++)
        printf("monitor %d,%d %dx%d%s\n", ms.m[i].x, ms.m[i].y, ms.m[i].w, ms.m[i].h, ms.m[i].primary ? " primary" : "");
    if (!conn) return;
    struct plh_input_layout_reply r; uint32_t rl = 0;
    memset(&r, 0, sizeof r);
    int st = plh_conn_request(conn, PLH_MSG_INPUT_SET_LAYOUT, &ms,
                              (uint32_t)(sizeof ms.h + ms.h.count * sizeof ms.m[0]), &r, sizeof r, &rl, 3000);
    printf("layout status=%d/%d translation=%+d,%+d kwin_desktop=%d,%d %dx%d\n", st, r.status, r.tx, r.ty,
           r.desk_x, r.desk_y, r.desk_w, r.desk_h);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int secs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dry = 1;
        else if (!strcmp(argv[i], "--passthrough")) passthrough = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
        else { fprintf(stderr, "usage: plh-input-hook.exe [--dry-run] [--passthrough] [--seconds N] [-v]\n"); return 2; }
    }
    if (!dry) {
        struct plh_hello_reply hr;
        int st = plh_conn_open(&conn, PLH_CLIENT_INPUT, "plh-input-hook", &hr);
        if (st != PLH_OK) { fprintf(stderr, "plh-input-hook: bridge unavailable (%d)\n", st); return 1; }
        if (!(hr.services & (1u << PLH_SVC_INPUT))) { fprintf(stderr, "plh-input-hook: input service missing from the bridge\n"); return 1; }
    }
    send_layout();
    int acc[3] = { 0, 0, 0 };
    if (!SystemParametersInfoW(SPI_SETMOUSE, 0, acc, 0)) fprintf(stderr, "SPI_SETMOUSE failed\n");
    HINSTANCE hi = GetModuleHandleW(NULL);
    hm = SetWindowsHookExW(WH_MOUSE_LL, mouse_ll, hi, 0);
    hk = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_ll, hi, 0);
    if (!hm || !hk) { fprintf(stderr, "SetWindowsHookEx: %lu\n", GetLastError()); return 1; }
    printf("READY%s%s\n", dry ? " dry-run" : "", passthrough ? " passthrough" : "");
    fflush(stdout);
    if (secs > 0) SetTimer(NULL, 1, (UINT)secs * 1000, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER && !msg.hwnd) break;
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hm);
    UnhookWindowsHookEx(hk);
    if (conn) {
        plh_conn_request(conn, PLH_MSG_INPUT_RELEASE_ALL, NULL, 0, NULL, 0, NULL, 1000);
        plh_conn_close(conn);
    }
    printf("END sent=%ld failed=%ld\n", sent, failed);
    return 0;
}
