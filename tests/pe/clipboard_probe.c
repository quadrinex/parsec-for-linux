/* T5 — Windows clipboard (public user32 APIs):
 *   clipboard_probe.exe get              prints CF_UNICODETEXT
 *   clipboard_probe.exe set TEXT [ms]   sets TEXT, then stays alive ms (default 3000)
 * Output: "text=<...>" (UTF-8) or "text=(empty)". */
#include "plh_test.h"
#include <stdlib.h>

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) return 2;
    if (!wcscmp(argv[1], L"get")) {
        BOOL ok = FALSE;
        for (int i = 0; i < 20 && !(ok = OpenClipboard(NULL)); i++) Sleep(50);
        OK(ok, "OpenClipboard");
        HANDLE h = ok ? GetClipboardData(CF_UNICODETEXT) : NULL;
        const wchar_t *w = h ? GlobalLock(h) : NULL;
        char u8[4096] = "(empty)";
        if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof u8, NULL, NULL);
        plh_out("text=%s\n", u8);
        if (w) GlobalUnlock(h);
        if (ok) CloseClipboard();
        return plh_failures ? 1 : 0;
    }
    if (!wcscmp(argv[1], L"set") && argc >= 3) {
        size_t n = (wcslen(argv[2]) + 1) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
        memcpy(GlobalLock(g), argv[2], n); GlobalUnlock(g);
        BOOL ok = OpenClipboard(NULL);
        OK(ok, "OpenClipboard");
        if (!ok) return 1;
        EmptyClipboard();
        OK(SetClipboardData(CF_UNICODETEXT, g) != NULL, "SetClipboardData(CF_UNICODETEXT)");
        CloseClipboard();
        Sleep(argc > 3 ? (DWORD)wcstoul(argv[3], NULL, 10) : 3000);
        return plh_failures ? 1 : 0;
    }
    return 2;
}
