/* T4 — Input injection through the public Windows APIs (those Parsec
 * uses on the host side, to be confirmed by api-map):
 *
 *   sendinput_probe.exe click X Y [X Y ...]   absolute SendInput (VIRTUALDESK) + left click
 *   sendinput_probe.exe setpos X Y [X Y ...]  SetCursorPos + left click (mouse_event)
 *   sendinput_probe.exe key VK                SendInput key (press + release)
 *   sendinput_probe.exe wheel DELTA           SendInput wheel
 *
 * X, Y in Windows virtual screen coordinates. After each move,
 * prints GetCursorPos (what Windows believes). 300 ms pause between points. */
#include "plh_test.h"
#include <stdlib.h>

static LONG norm(LONG v, LONG origin, LONG size) {
    /* smallest normalized value n such that floor(n*size/65536) == v-origin */
    return (LONG)(((long long)(v - origin) * 65536 + size - 1) / size);
}

static void report(const char *what, LONG x, LONG y, UINT sent) {
    POINT p; GetCursorPos(&p);
    plh_out("%s target=%ld,%ld sent=%u cursor=%ld,%ld\n", what, x, y, sent, p.x, p.y);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { plh_out("usage: click|setpos X Y... | key VK | wheel D\n"); return 2; }
    LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    LONG vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    plh_out("virtual_screen=%ld,%ld,%ld,%ld\n", vx, vy, vw, vh);
    const wchar_t *mode = argv[1];
    if (!wcscmp(mode, L"click") || !wcscmp(mode, L"setpos")) {
        for (int i = 2; i + 1 < argc; i += 2) {
            LONG x = wcstol(argv[i], NULL, 10), y = wcstol(argv[i + 1], NULL, 10);
            UINT sent;
            if (!wcscmp(mode, L"click")) {
                INPUT in[3] = {0};
                in[0].type = INPUT_MOUSE;
                in[0].mi.dx = norm(x, vx, vw); in[0].mi.dy = norm(y, vy, vh);
                in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                in[2].type = INPUT_MOUSE; in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                sent = SendInput(1, &in[0], sizeof(INPUT));
                Sleep(50);
                report("move", x, y, sent);
                sent = SendInput(2, &in[1], sizeof(INPUT));
            } else {
                sent = SetCursorPos(x, y);
                Sleep(50);
                report("move", x, y, sent);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            }
            plh_out("click target=%ld,%ld sent=%u\n", x, y, sent);
            Sleep(300);
        }
    } else if (!wcscmp(mode, L"key")) {
        WORD vk = (WORD)wcstol(argv[2], NULL, 0);
        INPUT in[2] = {0};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
        in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        UINT sent = SendInput(2, in, sizeof(INPUT));
        plh_out("key vk=0x%02x scan=0x%02x sent=%u\n", vk, in[0].ki.wScan, sent);
    } else if (!wcscmp(mode, L"wheel")) {
        INPUT in = {0};
        in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)wcstol(argv[2], NULL, 10);
        plh_out("wheel delta=%ld sent=%u\n", (long)(int)in.mi.mouseData, SendInput(1, &in, sizeof in));
    } else return 2;
    Sleep(300);
    return 0;
}
