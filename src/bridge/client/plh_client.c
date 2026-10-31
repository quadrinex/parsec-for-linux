/* plh_client.c — see plh_client.h. Owner: capture. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "plh_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
typedef SRWLOCK lock_t;
typedef CONDITION_VARIABLE cond_t;
#define lock_init(l) InitializeSRWLock(l)
#define lock(l) AcquireSRWLockExclusive(l)
#define unlock(l) ReleaseSRWLockExclusive(l)
#define cond_init(c) InitializeConditionVariable(c)
#define cond_broadcast(c) WakeAllConditionVariable(c)
static int cond_wait_ms(cond_t *c, lock_t *l, int ms)
{
    return SleepConditionVariableSRW(c, l, ms < 0 ? INFINITE : (DWORD)ms, 0) ? 0 : -1;
}
#define sock_close closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
typedef pthread_mutex_t lock_t;
typedef pthread_cond_t cond_t;
#define lock_init(l) pthread_mutex_init(l, NULL)
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
static void cond_init(cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
#define cond_broadcast(c) pthread_cond_broadcast(c)
static int cond_wait_ms(cond_t *c, lock_t *l, int ms)
{
    if (ms < 0) return pthread_cond_wait(c, l);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, l, &ts);
}
#define sock_close close
#endif

#define MAX_PENDING 16

struct pending {
    uint32_t seq;
    int      done;
    int      status;
    void    *buf;
    uint32_t cap;
    uint32_t len;
};

struct plh_conn {
    sock_t       s;
    int          tcp;
    lock_t       send_lock;
    lock_t       state_lock;   /* pending[], reading, dead */
    cond_t       cond;
    uint32_t     next_seq;
    struct pending *pending[MAX_PENDING];
    int          reading;      /* a thread is currently reading */
    int          dead;
    int          reader_thread;
    plh_event_cb cb;
    void        *cb_user;
    uint8_t     *rbuf;
    uint32_t     rcap;
#ifdef _WIN32
    HANDLE       thread;
#else
    pthread_t    thread;
#endif
};

uint64_t plh_client_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ paths */
static int bridge_dir(char *out, size_t cap)
{
    const char *d = getenv("PLH_BRIDGE_DIR");
    if (d && *d) { snprintf(out, cap, "%s", d); return 0; }
    const char *x = getenv("XDG_RUNTIME_DIR");
    /* Wine renames XDG_* to WINE_HOST_XDG_* in the Windows environment (observed, Wine 11.20) */
    if (!x || !*x) x = getenv("WINE_HOST_XDG_RUNTIME_DIR");
    if (!x || !*x) return -1;
    snprintf(out, cap, "%s/%s", x, PLH_RUNTIME_SUBDIR);
    return 0;
}

#ifdef _WIN32
/* "/run/user/1000/x" -> "Z:\run\user\1000\x" */
static void unix_to_dos(const char *u, char *out, size_t cap)
{
    snprintf(out, cap, "Z:%s", u);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}
#endif

static int read_small_file(const char *unix_path, void *buf, size_t cap, size_t *len)
{
#ifdef _WIN32
    char dos[600];
    unix_to_dos(unix_path, dos, sizeof dos);
    HANDLE h = CreateFileA(dos, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)cap, &n, NULL);
    CloseHandle(h);
    if (!ok) return -1;
    *len = n;
#else
    int fd = open(unix_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap);
    close(fd);
    if (n < 0) return -1;
    *len = (size_t)n;
#endif
    return 0;
}

/* ------------------------------------------------------------------ raw socket */
static int send_all(sock_t s, const void *p, size_t n)
{
    const char *b = p;
    while (n) {
#ifdef _WIN32
        int r = send(s, b, (int)n, 0);
        if (r <= 0) return -1;
#else
        ssize_t r = send(s, b, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
#endif
        b += r; n -= (size_t)r;
    }
    return 0;
}

static int recv_all(sock_t s, void *p, size_t n)
{
    char *b = p;
    while (n) {
#ifdef _WIN32
        int r = recv(s, b, (int)n, 0);
        if (r <= 0) return -1;
#else
        ssize_t r = recv(s, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
#endif
        b += r; n -= (size_t)r;
    }
    return 0;
}

/* waits for readable data; 1 = ready, 0 = timeout, <0 error */
static int wait_readable(sock_t s, int ms)
{
#ifdef _WIN32
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(0, &rs, NULL, NULL, ms < 0 ? NULL : &tv);
    return r < 0 ? -1 : r;
#else
    struct pollfd pfd = { s, POLLIN, 0 };
    int r;
    do r = poll(&pfd, 1, ms); while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : r;
#endif
}

static int send_msg(struct plh_conn *c, uint16_t type, uint32_t seq, const void *p, uint32_t len)
{
    struct plh_msg_hdr h = { PLH_PROTO_MAGIC, type, 0, len, seq };
    lock(&c->send_lock);
    int r = send_all(c->s, &h, sizeof h);
    if (!r && len) r = send_all(c->s, p, len);
    unlock(&c->send_lock);
    return r ? PLH_E_IO : PLH_OK;
}

/* reads a full message into c->rbuf (called by a single reader at a time) */
static int read_msg(struct plh_conn *c, struct plh_msg_hdr *h)
{
    if (recv_all(c->s, h, sizeof *h) < 0) return PLH_E_IO;
    if (h->magic != PLH_PROTO_MAGIC || h->size > PLH_MAX_PAYLOAD) return PLH_E_PROTO;
    if (h->size + 8 > c->rcap) {
        uint8_t *nb = realloc(c->rbuf, h->size + 8);
        if (!nb) return PLH_E_NOMEM;
        c->rbuf = nb; c->rcap = h->size + 8;
    }
    if (h->size && recv_all(c->s, c->rbuf, h->size) < 0) return PLH_E_IO;
    /* SCM_RIGHTS fds (native) are not collected by this library */
    return PLH_OK;
}

/* dispatches a message that was read; called WITHOUT state_lock */
static void deliver(struct plh_conn *c, const struct plh_msg_hdr *h)
{
    if ((h->flags & PLH_F_REPLY) && h->seq) {
        lock(&c->state_lock);
        for (int i = 0; i < MAX_PENDING; i++) {
            struct pending *p = c->pending[i];
            if (p && p->seq == h->seq && !p->done) {
                p->len = h->size;
                if (h->type == PLH_MSG_ERROR) {
                    struct plh_error e = { .status = PLH_E_PROTO };
                    memcpy(&e, c->rbuf, h->size < sizeof e ? h->size : sizeof e);
                    p->status = e.status ? e.status : PLH_E_PROTO;
                } else {
                    p->status = PLH_OK;
                }
                if (p->buf) memcpy(p->buf, c->rbuf, h->size < p->cap ? h->size : p->cap);
                p->done = 1;
                cond_broadcast(&c->cond);
                unlock(&c->state_lock);
                return;
            }
        }
        unlock(&c->state_lock);
    }
    if (c->cb) c->cb(c->cb_user, h, c->rbuf);
}

/* ------------------------------------------------------------------ reader */
#ifdef _WIN32
static DWORD WINAPI reader_main(void *arg)
#else
static void *reader_main(void *arg)
#endif
{
    struct plh_conn *c = arg;
    for (;;) {
        struct plh_msg_hdr h;
        if (read_msg(c, &h) != PLH_OK) break;
        deliver(c, &h);
    }
    lock(&c->state_lock);
    c->dead = 1;
    cond_broadcast(&c->cond);
    unlock(&c->state_lock);
    if (c->cb) {
        struct plh_msg_hdr h = { PLH_PROTO_MAGIC, PLH_MSG_BYE, PLH_F_EVENT, 0, 0 };
        c->cb(c->cb_user, &h, NULL); /* signals the disconnection */
    }
    return 0;
}

int plh_conn_start_reader(struct plh_conn *c)
{
    lock(&c->state_lock);
    if (c->reader_thread || c->reading) { unlock(&c->state_lock); return PLH_E_BUSY; }
    c->reader_thread = 1;
    unlock(&c->state_lock);
#ifdef _WIN32
    c->thread = CreateThread(NULL, 0, reader_main, c, 0, NULL);
    if (!c->thread) { c->reader_thread = 0; return PLH_E_NOMEM; }
#else
    if (pthread_create(&c->thread, NULL, reader_main, c)) { c->reader_thread = 0; return PLH_E_NOMEM; }
#endif
    return PLH_OK;
}

/* ------------------------------------------------------------------ API */
int plh_conn_is_tcp(const struct plh_conn *c) { return c->tcp; }

void plh_conn_set_event_cb(struct plh_conn *c, plh_event_cb cb, void *user)
{
    c->cb = cb;
    c->cb_user = user;
}

int plh_conn_send(struct plh_conn *c, uint16_t type, const void *payload, uint32_t len)
{
    return send_msg(c, type, 0, payload, len);
}

int plh_conn_request(struct plh_conn *c, uint16_t type, const void *req, uint32_t req_len,
                     void *reply, uint32_t reply_cap, uint32_t *reply_len, int timeout_ms)
{
    struct pending p = { 0 };
    p.buf = reply; p.cap = reply_cap;
    int slot = -1;
    lock(&c->state_lock);
    if (c->dead) { unlock(&c->state_lock); return PLH_E_IO; }
    for (int i = 0; i < MAX_PENDING; i++) if (!c->pending[i]) { slot = i; break; }
    if (slot < 0) { unlock(&c->state_lock); return PLH_E_BUSY; }
    if (++c->next_seq == 0) c->next_seq = 1;
    p.seq = c->next_seq;
    c->pending[slot] = &p;
    unlock(&c->state_lock);

    int r = send_msg(c, type, p.seq, req, req_len);
    uint64_t deadline = timeout_ms < 0 ? 0 : plh_client_now_ns() + (uint64_t)timeout_ms * 1000000ull;

    lock(&c->state_lock);
    while (!r && !p.done && !c->dead) {
        int left = -1;
        if (timeout_ms >= 0) {
            uint64_t now = plh_client_now_ns();
            if (now >= deadline) { r = PLH_E_AGAIN; break; }
            left = (int)((deadline - now) / 1000000ull) + 1;
        }
        if (c->reader_thread || c->reading) {
            cond_wait_ms(&c->cond, &c->state_lock, left);
            continue;
        }
        /* no reader: read ourselves */
        c->reading = 1;
        unlock(&c->state_lock);
        int w = wait_readable(c->s, left);
        struct plh_msg_hdr h;
        int rr = PLH_OK;
        if (w > 0) {
            rr = read_msg(c, &h);
            if (rr == PLH_OK) deliver(c, &h);
        } else if (w < 0) {
            rr = PLH_E_IO;
        }
        lock(&c->state_lock);
        c->reading = 0;
        if (rr != PLH_OK) c->dead = 1;
        cond_broadcast(&c->cond);
    }
    if (!r && !p.done) r = PLH_E_IO;
    if (!r) r = p.status;
    c->pending[slot] = NULL;
    unlock(&c->state_lock);
    if (reply_len) *reply_len = p.len;
    return r;
}

int plh_conn_poll(struct plh_conn *c, int timeout_ms)
{
    lock(&c->state_lock);
    if (c->reader_thread || c->reading || c->dead) { unlock(&c->state_lock); return c->dead ? PLH_E_IO : PLH_E_BUSY; }
    c->reading = 1;
    unlock(&c->state_lock);
    int count = 0, rr = PLH_OK;
    uint64_t deadline = plh_client_now_ns() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms) * 1000000ull;
    for (;;) {
        uint64_t now = plh_client_now_ns();
        int left = timeout_ms < 0 ? -1 : (now >= deadline ? 0 : (int)((deadline - now) / 1000000ull));
        int w = wait_readable(c->s, count ? 0 : left);
        if (w <= 0) { if (w < 0) rr = PLH_E_IO; break; }
        struct plh_msg_hdr h;
        rr = read_msg(c, &h);
        if (rr != PLH_OK) break;
        deliver(c, &h);
        count++;
    }
    lock(&c->state_lock);
    c->reading = 0;
    if (rr != PLH_OK) c->dead = 1;
    cond_broadcast(&c->cond);
    unlock(&c->state_lock);
    return rr != PLH_OK ? rr : count;
}

void plh_conn_log(struct plh_conn *c, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    send_msg(c, PLH_MSG_LOG, 0, buf, (uint32_t)n);
}

int plh_conn_open(struct plh_conn **out, uint32_t client_kind, const char *name, struct plh_hello_reply *reply_out)
{
    char dir[400], path[512];
    struct plh_hello hello = { .version_major = PLH_PROTO_VERSION_MAJOR, .version_minor = PLH_PROTO_VERSION_MINOR,
                               .client_kind = client_kind };
    *out = NULL;
    if (bridge_dir(dir, sizeof dir) < 0) return PLH_E_NOENT;
    snprintf(hello.name, sizeof hello.name, "%s", name ? name : "?");

    struct plh_conn *c = calloc(1, sizeof *c);
    if (!c) return PLH_E_NOMEM;
    c->s = BAD_SOCK;
    lock_init(&c->send_lock);
    lock_init(&c->state_lock);
    cond_init(&c->cond);

#ifdef _WIN32
    hello.pid = GetCurrentProcessId();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { free(c); return PLH_E_IO; }
    int use_tcp = 1;
#else
    hello.pid = (uint32_t)getpid();
    const char *ft = getenv("PLH_BRIDGE_FORCE_TCP");
    int use_tcp = ft && *ft == '1';
#endif
    int r = PLH_E_IO;
    if (use_tcp) {
        char portbuf[16] = { 0 };
        size_t len = 0;
        snprintf(path, sizeof path, "%s/%s", dir, PLH_PORT_FILE);
        if (read_small_file(path, portbuf, sizeof portbuf - 1, &len) < 0) { r = PLH_E_NOENT; goto fail; }
        int port = atoi(portbuf);
        snprintf(path, sizeof path, "%s/%s", dir, PLH_TOKEN_FILE);
        if (read_small_file(path, hello.token, sizeof hello.token, &len) < 0 || len != PLH_TOKEN_SIZE) {
            r = PLH_E_AUTH;
            goto fail;
        }
        c->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c->s == BAD_SOCK) goto fail;
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(c->s, (struct sockaddr *)&a, sizeof a) != 0) { r = PLH_E_NOENT; goto fail; }
        int one = 1;
        setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        c->tcp = 1;
    } else {
#ifndef _WIN32
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(path, sizeof path, "%s/%s", dir, PLH_SOCKET_NAME);
        if (strlen(path) >= sizeof a.sun_path) goto fail;
        strcpy(a.sun_path, path);
        c->s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (c->s == BAD_SOCK) goto fail;
        if (connect(c->s, (struct sockaddr *)&a, sizeof a) != 0) { r = PLH_E_NOENT; goto fail; }
#endif
    }

    struct plh_hello_reply rep;
    memset(&rep, 0, sizeof rep);
    uint32_t rl = 0;
    r = plh_conn_request(c, PLH_MSG_HELLO, &hello, sizeof hello, &rep, sizeof rep, &rl, 5000);
    memset(hello.token, 0, sizeof hello.token);
    if (r == PLH_OK) r = rep.status;
    if (r != PLH_OK) goto fail;
    if (reply_out) *reply_out = rep;
    c->dead = 0;
    *out = c;
    return PLH_OK;
fail:
    if (c->s != BAD_SOCK) sock_close(c->s);
    free(c->rbuf);
    free(c);
    return r;
}

void plh_conn_close(struct plh_conn *c)
{
    if (!c) return;
    send_msg(c, PLH_MSG_BYE, 0, NULL, 0);
#ifdef _WIN32
    shutdown(c->s, SD_BOTH);
    if (c->reader_thread && c->thread) { WaitForSingleObject(c->thread, 2000); CloseHandle(c->thread); }
#else
    shutdown(c->s, SHUT_RDWR);
    if (c->reader_thread) pthread_join(c->thread, NULL);
#endif
    sock_close(c->s);
    free(c->rbuf);
    free(c);
}

/* ------------------------------------------------------------------ shm */
int plh_shm_map(struct plh_shm_map *m, const char *name, uint64_t size)
{
    memset(m, 0, sizeof *m);
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) return PLH_E_INVAL;
#ifdef _WIN32
    char path[300];
    snprintf(path, sizeof path, "Z:\\dev\\shm\\%s", name);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return PLH_E_NOENT;
    HANDLE mp = CreateFileMappingA(f, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!mp) { CloseHandle(f); return PLH_E_IO; }
    void *p = MapViewOfFile(mp, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
    if (!p) { CloseHandle(mp); CloseHandle(f); return PLH_E_IO; }
    m->priv[0] = f;
    m->priv[1] = mp;
#else
    char path[300];
    snprintf(path, sizeof path, "/dev/shm/%s", name);
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return PLH_E_NOENT;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return PLH_E_IO;
#endif
    m->ptr = p;
    m->size = size;
    return PLH_OK;
}

void plh_shm_unmap(struct plh_shm_map *m)
{
    if (!m->ptr) return;
#ifdef _WIN32
    UnmapViewOfFile(m->ptr);
    CloseHandle((HANDLE)m->priv[1]);
    CloseHandle((HANDLE)m->priv[0]);
#else
    munmap(m->ptr, m->size);
#endif
    memset(m, 0, sizeof *m);
}
