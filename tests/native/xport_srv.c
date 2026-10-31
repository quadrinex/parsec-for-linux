/* Native server of the independent reproduction of D-005 (PE<->native transport).
 *   xport_srv <dir>   writes <dir>/port and <dir>/shm (name of the /dev/shm file)
 * Protocol (TCP 127.0.0.1, TCP_NODELAY), 8-byte messages {char op; pad; uint32 i} :
 *   'P' i : ping → echo
 *   'N' i : native fills the whole 1080p BGRA frame with pattern(i), then replies
 *   'W' i : the PE filled the frame with pattern(i); native checks everything, replies 'K' or 'X'
 *   'Q'   : end. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define FSZ (1920u * 1080u * 4u)
static uint32_t pat(uint32_t i, uint32_t k) { return (i * 2654435761u) ^ (k * 40503u); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char p[512], name[64];
    snprintf(name, sizeof name, "plh-critique-xport-%d", getpid());
    snprintf(p, sizeof p, "/dev/shm/%s", name);
    int fd = open(p, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 || ftruncate(fd, FSZ)) { perror("shm"); return 1; }
    uint32_t *m = mmap(NULL, FSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (bind(s, (void *)&a, sizeof a) || listen(s, 1) || getsockname(s, (void *)&a, &al)) { perror("tcp"); return 1; }
    snprintf(p, sizeof p, "%s/shm", argv[1]); FILE *f = fopen(p, "w"); fprintf(f, "%s", name); fclose(f);
    snprintf(p, sizeof p, "%s/port", argv[1]); f = fopen(p, "w"); fprintf(f, "%d", ntohs(a.sin_port)); fclose(f);
    int k = accept(s, NULL, NULL);
    setsockopt(k, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    unsigned char b[8]; int nbad = 0, nw = 0, nn = 0;
    while (recv(k, b, 8, MSG_WAITALL) == 8 && b[0] != 'Q') {
        uint32_t i; memcpy(&i, b + 4, 4);
        if (b[0] == 'N') { for (uint32_t x = 0; x < FSZ / 4; x++) m[x] = pat(i, x); nn++; }
        if (b[0] == 'W') { int ok = 1; for (uint32_t x = 0; x < FSZ / 4; x++) if (m[x] != pat(i, x)) { ok = 0; break; }
                           b[0] = ok ? 'K' : 'X'; nbad += !ok; nw++; }
        if (send(k, b, 8, 0) != 8) break;
    }
    printf("srv native->PE=%d PE->native=%d PE->native_corrupted=%d\n", nn, nw, nbad);
    snprintf(p, sizeof p, "/dev/shm/%s", name); unlink(p);
    return nbad ? 1 : 0;
}
