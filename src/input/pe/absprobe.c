/* absprobe.c — spike S3: does an absolute SendInput outside the virtual screen reach a WH_MOUSE_LL hook
 * unclamped? Installs its own LL hook that SWALLOWS every injected move (returns 1), so nothing is
 * forwarded. Run it ONLY in a throwaway WINEPREFIX (own wineserver), never next to plh-input-hook.
 *   absprobe.exe X Y [X Y...]   points in virtual-screen pixels (may lie outside the screen) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static LONG vx, vy, vw, vh;
static HHOOK hh;
static volatile LONG seen;
static LRESULT CALLBACK ll(int code, WPARAM wp, LPARAM lp)
{
    if (code != HC_ACTION) return CallNextHookEx(hh, code, wp, lp);
    const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
    if (!(m->flags & LLMHF_INJECTED)) return CallNextHookEx(hh, code, wp, lp);
    printf("  hook: wp=0x%x pt=%ld,%ld flags=0x%lx\n", (unsigned)wp, m->pt.x, m->pt.y, m->flags);
    fflush(stdout);
    InterlockedIncrement(&seen);
    return 1; /* swallowed */
}
static LONG norm(LONG v, LONG o, LONG s) { return (LONG)(((long long)(v - o) * 65536 + s - 1) / s); }
static int argc_; static char **argv_;
static DWORD WINAPI sender(LPVOID p)
{
    (void)p;
    Sleep(300);
    for (int i = 1; i + 1 < argc_; i += 2) {
        LONG x = atol(argv_[i]), y = atol(argv_[i + 1]);
        INPUT in = { .type = INPUT_MOUSE };
        in.mi.dx = norm(x, vx, vw); in.mi.dy = norm(y, vy, vh);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        printf("send px=%ld,%ld norm=%ld,%ld\n", x, y, in.mi.dx, in.mi.dy); fflush(stdout);
        SendInput(1, &in, sizeof in); Sleep(150);
    }
    Sleep(300);
    return 0;
}
int main(int argc, char **argv)
{
    argc_ = argc; argv_ = argv;
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN); vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN); vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    printf("virtual=%ld,%ld %ldx%ld\n", vx, vy, vw, vh);
    hh = SetWindowsHookExW(WH_MOUSE_LL, ll, GetModuleHandleW(NULL), 0);
    if (!hh) { printf("hook failed %lu\n", GetLastError()); return 1; }
    SetTimer(NULL, 1, 4000 + 200 * argc, NULL);
    CreateThread(NULL, 0, sender, NULL, 0, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { if (msg.message == WM_TIMER) break; DispatchMessageW(&msg); }
    printf("seen=%ld\n", seen);
    return 0;
}
