/* winctl.exe — tool to observe/drive windows INSIDE the Wine prefix.
 * Acts only on the prefix's Win32 windows (public Win32 APIs), never on the KDE desktop.
 *   winctl list | listall               : visible (or all) top-level windows + children
 *   winctl click "<title>" "<button>"   : BM_CLICK on the button whose text contains <button>
 *                                          in the window whose title contains <title>
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -municode -o winctl.exe winctl.c
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static const wchar_t *g_title, *g_button;
static HWND g_found;

static BOOL CALLBACK child_list(HWND h, LPARAM depth)
{
    wchar_t cls[128], txt[256];
    GetClassNameW(h, cls, 128);
    GetWindowTextW(h, txt, 256);
    wprintf(L"%*s  child %p class=%ls text=\"%ls\" vis=%d en=%d\n", (int)depth, L"", (void *)h, cls, txt,
            IsWindowVisible(h), IsWindowEnabled(h));
    return TRUE;
}

static BOOL CALLBACK top_list(HWND h, LPARAM l)
{
    (void)l;
    wchar_t cls[128], txt[256];
    DWORD pid = 0;
    RECT r;
    if (!l && !IsWindowVisible(h)) return TRUE;
    GetClassNameW(h, cls, 128);
    GetWindowTextW(h, txt, 256);
    GetWindowRect(h, &r);
    GetWindowThreadProcessId(h, &pid);
    wprintf(L"top %p pid=%lu class=%ls text=\"%ls\" rect=%ld,%ld-%ld,%ld vis=%d\n", (void *)h, pid, cls, txt,
            r.left, r.top, r.right, r.bottom, IsWindowVisible(h));
    EnumChildWindows(h, child_list, 2);
    return TRUE;
}

static BOOL CALLBACK child_find(HWND h, LPARAM l)
{
    (void)l;
    wchar_t txt[256];
    GetWindowTextW(h, txt, 256);
    if (wcsstr(txt, g_button) && IsWindowVisible(h)) { g_found = h; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK top_find(HWND h, LPARAM l)
{
    (void)l;
    wchar_t txt[256];
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowTextW(h, txt, 256);
    if (wcsstr(txt, g_title)) EnumChildWindows(h, child_find, 0);
    return g_found == NULL;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc >= 2 && (!wcscmp(argv[1], L"list") || !wcscmp(argv[1], L"listall"))) {
        EnumWindows(top_list, !wcscmp(argv[1], L"listall"));
        return 0;
    }
    if (argc >= 4 && !wcscmp(argv[1], L"click")) {
        g_title = argv[2];
        g_button = argv[3];
        EnumWindows(top_find, 0);
        if (!g_found) { fwprintf(stderr, L"button not found\n"); return 1; }
        if (!IsWindowEnabled(g_found)) { fwprintf(stderr, L"button disabled\n"); return 2; }
        PostMessageW(g_found, BM_CLICK, 0, 0); /* Post: does not block if the page starts a long operation */
        wprintf(L"clicked %p\n", (void *)g_found);
        return 0;
    }
    fwprintf(stderr, L"usage: winctl list | winctl click <title> <button>\n");
    return 64;
}
