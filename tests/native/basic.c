/* Basic native test: the native chain builds, and the primitives the bridge
 * will need (AF_UNIX + shared memfd + fd passing via SCM_RIGHTS)
 * work on this machine. Output: "ok"/"not ok" lines, exit code 0/1. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;
#define CHECK(c, msg) do { if (c) printf("ok - %s\n", msg); else { printf("not ok - %s\n", msg); failures++; } } while (0)

static int send_fd(int s, int fd) {
    char b = 'F'; struct iovec io = { &b, 1 };
    char ctl[CMSG_SPACE(sizeof(int))]; memset(ctl, 0, sizeof ctl);
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(s, &m, 0) == 1 ? 0 : -1;
}
static int recv_fd(int s) {
    char b; struct iovec io = { &b, 1 };
    char ctl[CMSG_SPACE(sizeof(int))];
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl };
    if (recvmsg(s, &m, 0) != 1) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd;
}

int main(void) {
    const size_t sz = 1920 * 1080 * 4;
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair AF_UNIX SEQPACKET");
    int mfd = memfd_create("plh-test", MFD_CLOEXEC);
    CHECK(mfd >= 0 && ftruncate(mfd, sz) == 0, "memfd_create + ftruncate 1080p BGRA");
    unsigned char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    CHECK(p != MAP_FAILED, "shared mmap");
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int fd = recv_fd(sv[1]);
        unsigned char *q = fd >= 0 ? mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (q == MAP_FAILED) _exit(2);
        for (size_t i = 0; i < sz; i += 4096) q[i] = (unsigned char)(i >> 12);
        _exit(0);
    }
    close(sv[1]);
    CHECK(send_fd(sv[0], mfd) == 0, "fd sent via SCM_RIGHTS");
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the child receives and maps the fd");
    int same = 1;
    for (size_t i = 0; i < sz; i += 4096) if (p[i] != (unsigned char)(i >> 12)) { same = 0; break; }
    CHECK(same, "child writes visible to the parent (shared memory)");
    return failures ? 1 : 0;
}
