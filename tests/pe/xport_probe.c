/* PE client of the independent reproduction of D-005, under the prefix's Wine:
 *   xport_probe.exe <port> <Z:\path\to\shm\file>
 * 1. AF_UNIX winsock: socket(AF_UNIX) → raw result (D-005: refused);
 * 2. TCP 127.0.0.1: 2000 pings, mean/p50/p99;
 * 3. shm (CreateFile Z: + CreateFileMapping): 300 1080p frames written by the
 *    native side and fully verified here, 300 written here and verified by the native side;
 *    delivery time of a frame (round trip + check). */
#include <winsock2.h>
#include <afunix.h>
#include "plh_test.h"
#include <stdint.h>
#include <stdlib.h>
#define FSZ (1920u * 1080u * 4u)
static uint32_t pat(uint32_t i, uint32_t k) { return (i * 2654435761u) ^ (k * 40503u); }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static SOCKET s;
static int xfer(char op, uint32_t i, char *rep) {
    char b[8] = { op }; memcpy(b + 4, &i, 4);
    if (send(s, b, 8, 0) != 8) return -1;
    int got = 0; while (got < 8) { int n = recv(s, b + got, 8 - got, 0); if (n <= 0) return -1; got += n; }
    if (rep) *rep = b[0];
    return 0;
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 3) return 2;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET u = socket(1 /* AF_UNIX */, SOCK_STREAM, 0);
    plh_out("af_unix socket=%s wsaerror=%d\n", u == INVALID_SOCKET ? "refused" : "ok", u == INVALID_SOCKET ? WSAGetLastError() : 0);
    if (u != INVALID_SOCKET && argc > 3) { /* connect to a native socket (Windows path) */
        struct sockaddr_un ua = { 0 }; ua.sun_family = AF_UNIX;
        WideCharToMultiByte(CP_ACP, 0, argv[3], -1, ua.sun_path, sizeof ua.sun_path, NULL, NULL);
        int r = connect(u, (struct sockaddr *)&ua, sizeof ua);
        int e = r ? WSAGetLastError() : 0, sent = -1;
        if (!r) sent = send(u, "PLHU", 4, 0);
        plh_out("af_unix connect=%s wsaerror=%d send=%d path=%s\n", r ? "failed" : "ok", e, sent, ua.sun_path);
    }
    if (u != INVALID_SOCKET) closesocket(u);
    s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((u_short)_wtoi(argv[1]));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    OK(connect(s, (void *)&a, sizeof a) == 0, "connect TCP 127.0.0.1:%ls (err %d)", argv[1], WSAGetLastError());
    BOOL one = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    enum { NP = 2000, NF = 300 };
    static double t[NP];
    for (int i = 0; i < NP; i++) { double t0 = plh_now(); if (xfer('P', i, NULL)) { OK(0, "ping %d", i); return 1; } t[i] = (plh_now() - t0) * 1e6; }
    double sum = 0; for (int i = 0; i < NP; i++) sum += t[i];
    qsort(t, NP, sizeof *t, cmpd);
    plh_out("tcp_rtt_us n=%d mean=%.1f p50=%.1f p99=%.1f max=%.1f\n", NP, sum / NP, t[NP / 2], t[NP * 99 / 100], t[NP - 1]);
    HANDLE h = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    OK(h != INVALID_HANDLE_VALUE, "CreateFile %ls (err %lu)", argv[2], GetLastError());
    HANDLE mp = h != INVALID_HANDLE_VALUE ? CreateFileMappingW(h, NULL, PAGE_READWRITE, 0, 0, NULL) : NULL;
    uint32_t *m = mp ? MapViewOfFile(mp, FILE_MAP_ALL_ACCESS, 0, 0, FSZ) : NULL;
    OK(m != NULL, "CreateFileMapping + MapViewOfFile (err %lu)", GetLastError());
    if (!m) { xfer('Q', 0, NULL); return 1; }
    int bad_in = 0, bad_out = 0; double tin = 0, tout = 0;
    for (uint32_t i = 1; i <= NF; i++) {
        double t0 = plh_now();
        xfer('N', i, NULL);
        for (uint32_t x = 0; x < FSZ / 4; x++) if (m[x] != pat(i, x)) { bad_in++; break; }
        tin += plh_now() - t0;
        t0 = plh_now();
        for (uint32_t x = 0; x < FSZ / 4; x++) m[x] = pat(i + 100000, x);
        char r = 0; xfer('W', i + 100000, &r); if (r != 'K') bad_out++;
        tout += plh_now() - t0;
    }
    plh_out("shm native->PE frames=%d corrupted=%d ms_per_frame=%.2f\n", NF, bad_in, tin * 1e3 / NF);
    plh_out("shm PE->native frames=%d corrupted=%d ms_per_frame=%.2f\n", NF, bad_out, tout * 1e3 / NF);
    OK(bad_in == 0 && bad_out == 0, "1080p frames intact in both directions");
    xfer('Q', 0, NULL);
    return plh_failures ? 1 : 0;
}
