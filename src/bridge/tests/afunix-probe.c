/* afunix-probe.exe — checks whether the current Wine accepts AF_UNIX in winsock.
 * Prints "AFUNIX socket=OK|ERR <code>". System Wine 11.20: ERR 10047 (WSAEAFNOSUPPORT).
 * Owner: capture. */
#include <winsock2.h>
#include <stdio.h>
int main(void)
{
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { printf("AFUNIX socket=ERR %d\n", WSAGetLastError()); return 1; }
    printf("AFUNIX socket=OK\n");
    closesocket(s);
    return 0;
}
