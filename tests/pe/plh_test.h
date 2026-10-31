/* Common header of the PE test programs (Windows, built by mingw-w64,
 * run under Wine in runtime/prefix). Text output "key=value" and
 * ok / not ok lines, parsed by the tests/tNN-*.sh scripts. */
#ifndef PLH_TEST_H
#define PLH_TEST_H
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static int plh_failures __attribute__((unused));

__attribute__((unused)) static void plh_out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}
#define OK(c, ...) do { if (c) { plh_out("ok - "); } else { plh_out("not ok - "); plh_failures++; } \
                        plh_out(__VA_ARGS__); plh_out("\n"); } while (0)

__attribute__((unused)) static double plh_now(void) {
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* Version de Wine (NULL sous un vrai Windows). */
__attribute__((unused)) static const char *plh_wine_version(void) {
    typedef const char *(CDECL *fn_t)(void);
    HMODULE h = GetModuleHandleA("ntdll.dll");
    fn_t f = h ? (fn_t)(void *)GetProcAddress(h, "wine_get_version") : NULL;
    return f ? f() : NULL;
}

/* Path of the module actually loaded (to prove which DLL is used: Wine
 * builtin, DXVK, or project DLL). */
__attribute__((unused)) static void plh_print_module(const char *name) {
    HMODULE h = GetModuleHandleA(name);
    char path[MAX_PATH] = "(not loaded)";
    if (h) GetModuleFileNameA(h, path, sizeof path);
    plh_out("module %s=%s\n", name, path);
}
#endif
