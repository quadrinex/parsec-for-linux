/* ldrpath-test.exe — reproduces without Parsec the crash seen at parsecd startup:
 * LdrLoadDll called with a "search_path" that is actually a flags value,
 * LOAD_LIBRARY_SEARCH_SYSTEM32 (0x800), instead of a string pointer.
 * On Windows these small values are not dereferenced; on unpatched Wine 11.20,
 * LdrLoadDll calls wcslen(0x800) -> page fault.
 * Output: "OK <handle>" (exit code 0) or a crash / "FAIL status".
 * Compilation : x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o ldrpath-test.exe ldrpath-test.c
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

typedef NTSTATUS (NTAPI *LdrLoadDll_t)(PCWSTR, PULONG, PUNICODE_STRING, PVOID *);

int main(int argc, char **argv)
{
    ULONG_PTR path = 0x800; /* LOAD_LIBRARY_SEARCH_SYSTEM32 */
    if (argc > 1) path = strtoull(argv[1], NULL, 0);
    LdrLoadDll_t pLdrLoadDll = (LdrLoadDll_t)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrLoadDll");
    UNICODE_STRING name;
    WCHAR buf[] = L"version.dll";
    PVOID h = NULL;
    name.Buffer = buf;
    name.Length = sizeof(buf) - sizeof(WCHAR);
    name.MaximumLength = sizeof(buf);
    printf("LdrLoadDll(search_path=%#llx, version.dll)\n", (unsigned long long)path);
    fflush(stdout);
    NTSTATUS st = pLdrLoadDll((PCWSTR)path, NULL, &name, &h);
    if (st) { printf("FAIL status=%#lx\n", (unsigned long)st); return 1; }
    printf("OK %p\n", h);
    return 0;
}
