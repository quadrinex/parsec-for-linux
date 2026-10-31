/* plh-clipsync.exe : Windows clipboard gateway <-> /tmp/plh-clip files (UTF-8 text).
 * Runs in the dedicated prefix; plh-clipd.sh (Linux, Klipper) does the other half.
 * linux.txt : Linux -> Windows ; windows.txt : Windows -> Linux. Public APIs only. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static char DIR[MAX_PATH];

static char *readfile(const char *p, FILETIME *ft)
{
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, 0), r = 0;
    GetFileTime(h, 0, 0, ft);
    char *b = malloc(n + 1);
    if (!b || !ReadFile(h, b, n, &r, 0)) { CloseHandle(h); free(b); return NULL; }
    b[r] = 0; CloseHandle(h); return b;
}
static void writefile(const char *name, const char *data)
{
    char tmp[MAX_PATH], dst[MAX_PATH]; DWORD w;
    snprintf(tmp, sizeof tmp, "%s%s.tmp", DIR, name); snprintf(dst, sizeof dst, "%s%s", DIR, name);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, data, (DWORD)strlen(data), &w, 0); CloseHandle(h);
    MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}
static BOOL set_text(const char *u8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, 0, 0);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n * 2);
    if (!g) return FALSE;
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, GlobalLock(g), n); GlobalUnlock(g);
    if (!OpenClipboard(0)) { GlobalFree(g); return FALSE; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, g)) { CloseClipboard(); GlobalFree(g); return FALSE; }
    CloseClipboard(); return TRUE;
}
static char *get_text(void)
{
    char *out = NULL;
    if (!OpenClipboard(0)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        WCHAR *w = GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
            out = malloc(n); WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, 0, 0); GlobalUnlock(h);
        }
    }
    CloseClipboard(); return out;
}
int main(int argc, char **argv)
{
    DWORD seq, ticks = 0;
    /* usage: plh-clipsync.exe <Unix directory, e.g. /run/user/1000/plh-bridge/clip> [--seconds N] */
    int once = argc > 3 && !strcmp(argv[2], "--seconds") ? atoi(argv[3]) : 0;
    if (argc < 2 || argv[1][0] != '/') return 1;
    snprintf(DIR, sizeof DIR, "Z:%s/", argv[1]);
    for (char *c = DIR; *c; c++) if (*c == '/') *c = '\\';
    seq = GetClipboardSequenceNumber();
    for (;;) {
        char lp[MAX_PATH]; FILETIME ft; snprintf(lp, sizeof lp, "%slinux.txt", DIR);
        char *l = readfile(lp, &ft);
        if (l) {                       /* consumed then deleted: nothing stays on disk */
            DeleteFileA(lp);
            if (set_text(l)) seq = GetClipboardSequenceNumber();
            free(l);
        }
        DWORD s = GetClipboardSequenceNumber();
        if (s != seq) {
            seq = s;
            char *t = get_text();
            if (t) { writefile("windows.txt", t); free(t); }
        }
        Sleep(150); ticks += 150;
        if (once && ticks >= (DWORD)once * 1000) break;
    }
    return 0;
}
