/* injectprobe.c — injects input through public Windows APIs (input relay tests).
 *   injectprobe.exe abs X Y [X Y...]   SendInput MOUSEEVENTF_ABSOLUTE|VIRTUALDESK (virtual screen pixels)
 *   injectprobe.exe rel DX DY [...]    SendInput relative
 *   injectprobe.exe wheel D            SendInput wheel
 *   injectprobe.exe key VK             SendInput key (press+release), VK only
 *   injectprobe.exe sc SCAN [e0]       SendInput KEYEVENTF_SCANCODE
 *   injectprobe.exe legacy             mouse_event rel 3,4 + keybd_event F24
 *   injectprobe.exe click left|right|middle   SendInput button (press+release) at the current position
 *   injectprobe.exe hwheel D           SendInput horizontal wheel
 * 60 ms pause between events. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LONG norm(LONG v, LONG origin, LONG size) { return (LONG)(((long long)(v - origin) * 65536 + size - 1) / size); }

static void mouse(DWORD f, LONG dx, LONG dy, DWORD data)
{
    INPUT i; memset(&i, 0, sizeof i);
    i.type = INPUT_MOUSE; i.mi.dx = dx; i.mi.dy = dy; i.mi.dwFlags = f; i.mi.mouseData = data;
    printf("mouse f=0x%lx %ld,%ld data=%ld -> %u\n", f, dx, dy, (long)data, SendInput(1, &i, sizeof i));
    fflush(stdout); Sleep(60);
}
static void key(WORD vk, WORD sc, DWORD f)
{
    INPUT i; memset(&i, 0, sizeof i);
    i.type = INPUT_KEYBOARD; i.ki.wVk = vk; i.ki.wScan = sc; i.ki.dwFlags = f;
    printf("key vk=0x%x sc=0x%x f=0x%lx -> %u\n", vk, sc, f, SendInput(1, &i, sizeof i));
    fflush(stdout); Sleep(30);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    LONG vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    printf("virtual=%ld,%ld %ldx%ld\n", vx, vy, vw, vh);
    if (!strcmp(argv[1], "abs"))
        for (int i = 2; i + 1 < argc; i += 2)
            mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                  norm(atol(argv[i]), vx, vw), norm(atol(argv[i + 1]), vy, vh), 0);
    else if (!strcmp(argv[1], "rel"))
        for (int i = 2; i + 1 < argc; i += 2) mouse(MOUSEEVENTF_MOVE, atol(argv[i]), atol(argv[i + 1]), 0);
    else if (!strcmp(argv[1], "wheel") && argc > 2) mouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)atol(argv[2]));
    else if (!strcmp(argv[1], "key") && argc > 2) {
        WORD vk = (WORD)strtol(argv[2], NULL, 0); key(vk, 0, 0); key(vk, 0, KEYEVENTF_KEYUP);
    } else if (!strcmp(argv[1], "sc") && argc > 2) {
        WORD sc = (WORD)strtol(argv[2], NULL, 0);
        DWORD e = (argc > 3 && !strcmp(argv[3], "e0")) ? KEYEVENTF_EXTENDEDKEY : 0;
        key(0, sc, KEYEVENTF_SCANCODE | e); key(0, sc, KEYEVENTF_SCANCODE | e | KEYEVENTF_KEYUP);
    } else if (!strcmp(argv[1], "click") && argc > 2) {
        DWORD d = MOUSEEVENTF_LEFTDOWN, u = MOUSEEVENTF_LEFTUP;
        if (!strcmp(argv[2], "right")) { d = MOUSEEVENTF_RIGHTDOWN; u = MOUSEEVENTF_RIGHTUP; }
        else if (!strcmp(argv[2], "middle")) { d = MOUSEEVENTF_MIDDLEDOWN; u = MOUSEEVENTF_MIDDLEUP; }
        mouse(d, 0, 0, 0); mouse(u, 0, 0, 0);
    } else if (!strcmp(argv[1], "hwheel") && argc > 2) mouse(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)atol(argv[2]));
    else if (!strcmp(argv[1], "legacy")) {
        mouse_event(MOUSEEVENTF_MOVE, 3, 4, 0, 0); printf("mouse_event rel 3,4\n"); Sleep(60);
        keybd_event(0x87, 0, 0, 0); keybd_event(0x87, 0, KEYEVENTF_KEYUP, 0); printf("keybd_event F24\n"); Sleep(60);
    } else return 2;
    return 0;
}
