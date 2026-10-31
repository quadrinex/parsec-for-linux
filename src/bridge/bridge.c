/*
 * plh-bridge — native daemon of the parsec-for-linux bridge. Owner: capture.
 * Single-threaded epoll loop, listens on AF_UNIX (native clients, fds) and TCP 127.0.0.1 (PE under Wine, token).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "plh_bridge.h"

struct plh_config plh_cfg = { .cap_backend = "auto" };

/* ------------------------------------------------------------------ log / time */
uint64_t plh_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plh_log(int level, const char *mod, const char *fmt, ...)
{
    static const char *lv[] = { "ERR", "WRN", "INF", "DBG" };
    if (level > PLH_LOG_INFO + plh_cfg.verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    uint64_t t = plh_now_ns();
    fprintf(stderr, "[%llu.%03llu] %s %s: ", (unsigned long long)(t / 1000000000ull),
            (unsigned long long)(t / 1000000ull % 1000), lv[level & 3], mod);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ loop */
#define MAX_WATCH 1024
struct watch { int fd; plh_fd_cb cb; plh_timer_cb tcb; void *data; };
static struct watch *watches[MAX_WATCH]; /* indexed by fd */
static int epfd = -1;
static volatile sig_atomic_t quit;

int plh_loop_add_fd(int fd, uint32_t ev, plh_fd_cb cb, void *data)
{
    if (fd < 0 || fd >= MAX_WATCH) return -1;
    struct watch *w = calloc(1, sizeof *w);
    if (!w) return -1;
    w->fd = fd; w->cb = cb; w->data = data;
    struct epoll_event e = { .events = ev, .data.fd = fd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e) < 0) { free(w); return -1; }
    free(watches[fd]);
    watches[fd] = w;
    return 0;
}

int plh_loop_mod_fd(int fd, uint32_t ev)
{
    struct epoll_event e = { .events = ev, .data.fd = fd };
    return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
}

void plh_loop_del_fd(int fd)
{
    if (fd < 0 || fd >= MAX_WATCH) return;
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    free(watches[fd]);
    watches[fd] = NULL;
}

static void timer_fire(int fd, uint32_t ev, void *data)
{
    (void)ev;
    uint64_t n;
    if (read(fd, &n, sizeof n) != sizeof n) return;
    struct watch *w = watches[fd];
    if (w && w->tcb) w->tcb(data);
}

int plh_timer_add(uint64_t first_ns, uint64_t period_ns, plh_timer_cb cb, void *data)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) return -1;
    if (!first_ns) first_ns = 1;
    struct itimerspec its = {
        .it_value = { (time_t)(first_ns / 1000000000ull), (long)(first_ns % 1000000000ull) },
        .it_interval = { (time_t)(period_ns / 1000000000ull), (long)(period_ns % 1000000000ull) },
    };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0 || plh_loop_add_fd(tfd, EPOLLIN, timer_fire, data) < 0) {
        close(tfd);
        return -1;
    }
    watches[tfd]->tcb = cb;
    return tfd;
}

void plh_timer_del(int tfd)
{
    if (tfd < 0) return;
    plh_loop_del_fd(tfd);
    close(tfd);
}

/* ------------------------------------------------------------------ services */
static const struct plh_service *services[PLH_SVC_COUNT];
static int service_ok[PLH_SVC_COUNT];

void plh_service_register(const struct plh_service *svc)
{
    if (svc->id > 0 && svc->id < PLH_SVC_COUNT) services[svc->id] = svc;
}

/* ------------------------------------------------------------------ clients */
struct plh_client {
    int       fd;
    int       native;
    int       authed;
    uint32_t  id;
    uint32_t  kind;
    uint64_t  accepted_ns;
    char      name[PLH_NAME_MAX];
    uint8_t  *in;   size_t in_len, in_cap;
    uint8_t  *out;  size_t out_len, out_cap, out_off;
    void     *svc_data[PLH_SVC_COUNT];
    struct plh_client *next;
};
static struct plh_client *clients;
static uint32_t next_client_id = 1;
static uint8_t token[PLH_TOKEN_SIZE];
static int unauthed_count;
static int reserve_fd = -1;          /* spare fd to survive EMFILE */
#define MAX_UNAUTHED    32           /* simultaneous connections without a valid HELLO */
#define AUTH_TIMEOUT_NS 2000000000ull /* time allowed to send a valid HELLO */

static int listen_paused[2] = { -1, -1 };

static int token_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (int i = 0; i < PLH_TOKEN_SIZE; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

uint32_t plh_client_id(const struct plh_client *c) { return c->id; }
uint32_t plh_client_kind(const struct plh_client *c) { return c->kind; }
int plh_client_is_native(const struct plh_client *c) { return c->native; }
void *plh_client_get_data(struct plh_client *c, int svc) { return (svc >= 0 && svc < PLH_SVC_COUNT) ? c->svc_data[svc] : NULL; }
void plh_client_set_data(struct plh_client *c, int svc, void *d) { if (svc >= 0 && svc < PLH_SVC_COUNT) c->svc_data[svc] = d; }

static void client_close(struct plh_client *c);

static void client_flush(struct plh_client *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            c->out_len = c->out_off = 0;
            return;
        }
        c->out_off += (size_t)n;
    }
    if (c->out_off == c->out_len) {
        c->out_off = c->out_len = 0;
        plh_loop_mod_fd(c->fd, EPOLLIN);
    } else {
        plh_loop_mod_fd(c->fd, EPOLLIN | EPOLLOUT);
    }
}

static int out_reserve(struct plh_client *c, size_t extra)
{
    if (c->out_off && c->out_off == c->out_len) c->out_off = c->out_len = 0;
    if (c->out_len + extra <= c->out_cap) return 0;
    if (c->out_len + extra > 64u << 20) return -1; /* client that no longer reads */
    size_t cap = c->out_cap ? c->out_cap : 65536;
    while (cap < c->out_len + extra) cap *= 2;
    uint8_t *p = realloc(c->out, cap);
    if (!p) return -1;
    c->out = p; c->out_cap = cap;
    return 0;
}

static int send_fds_now(struct plh_client *c, const struct plh_msg_hdr *h, const void *p1, uint32_t s1,
                        const int *fds, int nfds)
{
    /* attached fds: drain the queue first, then a short blocking send of this message. */
    client_flush(c);
    if (c->out_len) return -1;
    char cbuf[CMSG_SPACE(sizeof(int) * 16)];
    if (nfds > 16) return -1;
    struct iovec iov[2] = { { (void *)h, sizeof *h }, { (void *)p1, s1 } };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = s1 ? 2 : 1, .msg_control = cbuf,
                        .msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds) };
    memset(cbuf, 0, sizeof cbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
    memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfds);
    size_t total = sizeof *h + s1;
    ssize_t n = sendmsg(c->fd, &m, MSG_NOSIGNAL);
    if (n < 0) return -1;
    if ((size_t)n < total) { /* remainder without fd */
        size_t done = (size_t)n;
        if (out_reserve(c, total - done) < 0) return -1;
        if (done < sizeof *h) {
            memcpy(c->out + c->out_len, (const uint8_t *)h + done, sizeof *h - done);
            c->out_len += sizeof *h - done;
            done = sizeof *h;
        }
        memcpy(c->out + c->out_len, (const uint8_t *)p1 + (done - sizeof *h), total - done);
        c->out_len += total - done;
        client_flush(c);
    }
    return 0;
}

int plh_send2(struct plh_client *c, uint16_t type, uint16_t flags, uint32_t seq,
              const void *p1, uint32_t s1, const void *p2, uint32_t s2)
{
    if (!c || c->fd < 0) return -1;
    struct plh_msg_hdr h = { PLH_PROTO_MAGIC, type, flags, s1 + s2, seq };
    if (out_reserve(c, sizeof h + s1 + s2) < 0) { PLH_WARN("bridge", "client %u: output queue full", c->id); return -1; }
    memcpy(c->out + c->out_len, &h, sizeof h); c->out_len += sizeof h;
    if (s1) { memcpy(c->out + c->out_len, p1, s1); c->out_len += s1; }
    if (s2) { memcpy(c->out + c->out_len, p2, s2); c->out_len += s2; }
    client_flush(c);
    return 0;
}

int plh_send(struct plh_client *c, uint16_t type, uint16_t flags, uint32_t seq,
             const void *payload, uint32_t size, const int *fds, int nfds)
{
    if (nfds > 0) {
        if (!c->native) return -1;
        struct plh_msg_hdr h = { PLH_PROTO_MAGIC, type, (uint16_t)(flags | PLH_F_HAS_FDS), size, seq };
        return send_fds_now(c, &h, payload, size, fds, nfds);
    }
    return plh_send2(c, type, flags, seq, payload, size, NULL, 0);
}

int plh_reply(struct plh_client *c, const struct plh_msg_hdr *req, const void *payload, uint32_t size)
{
    return plh_send2(c, req->type, PLH_F_REPLY, req->seq, payload, size, NULL, 0);
}

int plh_send_event(struct plh_client *c, uint16_t type, const void *payload, uint32_t size)
{
    return plh_send2(c, type, PLH_F_EVENT, 0, payload, size, NULL, 0);
}

int plh_reply_error(struct plh_client *c, const struct plh_msg_hdr *req, int32_t status, const char *fmt, ...)
{
    struct plh_error e = { .status = status, .req_type = req->type };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.message, sizeof e.message, fmt, ap);
    va_end(ap);
    return plh_send2(c, PLH_MSG_ERROR, PLH_F_REPLY, req->seq, &e, sizeof e, NULL, 0);
}

static void handle_core(struct plh_client *c, const struct plh_msg_hdr *h, const uint8_t *p)
{
    switch (h->type) {
    case PLH_MSG_HELLO: {
        struct plh_hello hello = { 0 };
        memcpy(&hello, p, h->size < sizeof hello ? h->size : sizeof hello);
        struct plh_hello_reply r = { .version_major = PLH_PROTO_VERSION_MAJOR,
                                     .version_minor = PLH_PROTO_VERSION_MINOR,
                                     .client_id = c->id, .daemon_time_ns = plh_now_ns(),
                                     .uid = (uint32_t)getuid() };
        if (hello.version_major != PLH_PROTO_VERSION_MAJOR) r.status = PLH_E_VERSION;
        else if (!c->native && !token_equal(hello.token, token)) r.status = PLH_E_AUTH;
        for (int i = 1; i < PLH_SVC_COUNT; i++)
            if (service_ok[i]) r.services |= 1u << i;
        r.services |= 1u;
        memcpy(c->name, hello.name, sizeof c->name);
        c->name[sizeof c->name - 1] = 0;
        c->kind = hello.client_kind;
        if (r.status) { /* disclose nothing to a rejected client */
            r.services = 0; r.uid = 0; r.client_id = 0; r.daemon_time_ns = 0;
        }
        plh_reply(c, h, &r, sizeof r);
        if (r.status) {
            PLH_WARN("bridge", "client %u (%s) rejected: %d", c->id, c->name, r.status);
            client_flush(c);
            client_close(c);
            return;
        }
        if (!c->authed) unauthed_count--;
        c->authed = 1;
        PLH_INFO("bridge", "client %u connected: '%s' type %u pid %u v%u.%u (%s)", c->id, c->name,
                 c->kind, hello.pid, hello.version_major, hello.version_minor, c->native ? "unix" : "tcp");
        break;
    }
    case PLH_MSG_PING: {
        struct plh_pong pong = { plh_now_ns() };
        plh_send2(c, PLH_MSG_PING, PLH_F_REPLY, h->seq, &pong, sizeof pong, p, h->size);
        break;
    }
    case PLH_MSG_LOG:
        PLH_INFO("client", "%u/%s: %.*s", c->id, c->name, (int)h->size, (const char *)p);
        break;
    case PLH_MSG_BYE:
        client_close(c);
        break;
    default:
        plh_reply_error(c, h, PLH_E_UNKNOWN, "type 0x%04x inconnu", h->type);
    }
}

static void dispatch(struct plh_client *c, const struct plh_msg_hdr *h, const uint8_t *p)
{
    int svc = PLH_SERVICE(h->type);
    if (!c->authed && h->type != PLH_MSG_HELLO) {
        PLH_WARN("bridge", "client %u: message 0x%04x before HELLO", c->id, h->type);
        client_close(c);
        return;
    }
    if (svc == PLH_SVC_CORE) { handle_core(c, h, p); return; }
    if (svc < PLH_SVC_COUNT && service_ok[svc] && services[svc]->handle) {
        services[svc]->handle(c, h, p);
        return;
    }
    if (h->seq) plh_reply_error(c, h, PLH_E_UNKNOWN, "service 0x%02x indisponible", svc);
}

static void client_io(int fd, uint32_t ev, void *data)
{
    struct plh_client *c = data;
    (void)fd;
    if (ev & EPOLLOUT) client_flush(c);
    if (ev & (EPOLLERR | EPOLLHUP) && !(ev & EPOLLIN)) { client_close(c); return; }
    if (!(ev & EPOLLIN)) return;
    for (;;) {
        if (c->in_cap - c->in_len < 65536) {
            size_t cap = c->in_cap ? c->in_cap * 2 : 131072;
            uint8_t *p = realloc(c->in, cap);
            if (!p) { client_close(c); return; }
            c->in = p; c->in_cap = cap;
        }
        ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (n == 0) { client_close(c); return; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            client_close(c);
            return;
        }
        c->in_len += (size_t)n;
        size_t off = 0;
        uint32_t id = c->id;
        while (c->in_len - off >= sizeof(struct plh_msg_hdr)) {
            struct plh_msg_hdr h;
            memcpy(&h, c->in + off, sizeof h);
            if (h.magic != PLH_PROTO_MAGIC || h.size > PLH_MAX_PAYLOAD) {
                PLH_WARN("bridge", "client %u: invalid frame (magic %08x size %u)", c->id, h.magic, h.size);
                client_close(c);
                return;
            }
            if (c->in_len - off < sizeof h + h.size) break;
            /* payload aligned to 8 for services */
            uint8_t stackbuf[4096] __attribute__((aligned(8)));
            uint8_t *pl = h.size <= sizeof stackbuf ? stackbuf : malloc(h.size);
            if (!pl) { client_close(c); return; }
            memcpy(pl, c->in + off + sizeof h, h.size);
            off += sizeof h + h.size;
            dispatch(c, &h, pl);
            if (pl != stackbuf) free(pl);
            /* the client may have been closed by the service */
            struct plh_client *it;
            for (it = clients; it && it != c; it = it->next) ;
            if (!it || it->id != id || c->fd < 0) return;
        }
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static struct plh_client *closing; /* deferred release */

static void client_close(struct plh_client *c)
{
    if (c->fd < 0) return;
    if (!c->authed) unauthed_count--;
    else PLH_INFO("bridge", "client %u (%s) disconnected", c->id, c->name);
    for (int i = 1; i < PLH_SVC_COUNT; i++)
        if (service_ok[i] && services[i]->client_gone) services[i]->client_gone(c);
    plh_loop_del_fd(c->fd);
    close(c->fd);
    c->fd = -1;
    struct plh_client **pp;
    for (pp = &clients; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    c->next = closing;
    closing = c;
}

static void auth_timeout_cb(void *d)
{
    (void)d;
    if (reserve_fd < 0) reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    for (int i = 0; i < 2; i++)
        if (listen_paused[i] >= 0) { plh_loop_mod_fd(listen_paused[i], EPOLLIN); listen_paused[i] = -1; }
    uint64_t now = plh_now_ns();
    struct plh_client *c = clients;
    while (c) {
        struct plh_client *n = c->next;
        if (!c->authed && now - c->accepted_ns > AUTH_TIMEOUT_NS) client_close(c);
        c = n;
    }
}

static void reap_closed(void)
{
    while (closing) {
        struct plh_client *c = closing;
        closing = c->next;
        free(c->in); free(c->out); free(c);
    }
}

static void accept_cb(int lfd, uint32_t ev, void *data)
{
    (void)ev;
    int native = data != NULL;
    for (int iter = 0; iter < 256; iter++) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if ((errno == EMFILE || errno == ENFILE) && reserve_fd >= 0) {
                /* out of descriptors: release the spare, accept and close, take the spare back */
                close(reserve_fd);
                fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
                if (fd >= 0) close(fd);
                reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
                PLH_WARN("bridge", "out of descriptors (EMFILE): connection refused");
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                /* no spare: stop listening, the timer re-arms it (avoids a busy epoll loop) */
                plh_loop_mod_fd(lfd, 0);
                listen_paused[native ? 1 : 0] = lfd;
            }
            return; /* EAGAIN or transient error */
        }
        if (native) {
            struct ucred cr; socklen_t l = sizeof cr;
            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) < 0 || cr.uid != getuid()) {
                PLH_WARN("bridge", "unix connection from another uid refused");
                close(fd);
                continue;
            }
        } else {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        struct plh_client *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->fd = fd; c->native = native; c->id = next_client_id++;
        c->accepted_ns = plh_now_ns();
        strcpy(c->name, "?");
        if (plh_loop_add_fd(fd, EPOLLIN, client_io, c) < 0) { close(fd); free(c); continue; }
        c->next = clients;
        clients = c;
        unauthed_count++;
        if (unauthed_count > MAX_UNAUTHED) {
            /* too many silent connections: evict the oldest unauthenticated one */
            struct plh_client *old = NULL;
            for (struct plh_client *it = clients; it; it = it->next)
                if (!it->authed && it->fd >= 0) old = it; /* list: most recent first */
            if (old) client_close(old);
        }
    }
}

/* ------------------------------------------------------------------ shm */
int plh_shm_create(struct plh_shm *shm, const char *tag, uint64_t size)
{
    static unsigned counter;
    memset(shm, 0, sizeof *shm);
    shm->fd = -1;
    for (int tries = 0; tries < 100; tries++) {
        snprintf(shm->name, sizeof shm->name, "plh-%u-%s-%d-%u", (unsigned)getuid(), tag, (int)getpid(), counter++);
        char path[128];
        snprintf(path, sizeof path, "/dev/shm/%s", shm->name);
        shm->fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (shm->fd >= 0) break;
        if (errno != EEXIST) return -1;
    }
    if (shm->fd < 0) return -1;
    if (ftruncate(shm->fd, (off_t)size) < 0) goto fail;
    shm->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->map == MAP_FAILED) { shm->map = NULL; goto fail; }
    shm->size = size;
    return 0;
fail:
    plh_shm_destroy(shm);
    return -1;
}

void plh_shm_destroy(struct plh_shm *shm)
{
    if (shm->map) munmap(shm->map, shm->size);
    if (shm->fd >= 0) close(shm->fd);
    if (shm->name[0]) {
        char path[128];
        snprintf(path, sizeof path, "/dev/shm/%s", shm->name);
        unlink(path);
    }
    memset(shm, 0, sizeof *shm);
    shm->fd = -1;
}

/* ------------------------------------------------------------------ main */
static void on_signal(int s) { (void)s; quit = 1; }

static int write_file(const char *dir, const char *name, const void *data, size_t len)
{
    char tmp[512], path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (write(fd, data, len) != (ssize_t)len) { close(fd); unlink(tmp); return -1; }
    close(fd);
    return rename(tmp, path);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: plh-bridge [-v] [--port N] [--runtime-dir DIR] [--capture-backend auto|portal|kwin|x11|test]\n"
            "  Listens on $XDG_RUNTIME_DIR/plh-bridge/bridge.sock (native) and 127.0.0.1:port (PE/Wine).\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "port", required_argument, 0, 'p' }, { "runtime-dir", required_argument, 0, 'r' },
        { "capture-backend", required_argument, 0, 'b' }, { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    static char rdir[256];
    int o;
    if (getenv("PLH_CAPTURE_BACKEND")) plh_cfg.cap_backend = getenv("PLH_CAPTURE_BACKEND");
    while ((o = getopt_long(argc, argv, "vp:r:b:h", opts, NULL)) != -1) {
        switch (o) {
        case 'v': plh_cfg.verbose++; break;
        case 'p': plh_cfg.tcp_port = atoi(optarg); break;
        case 'r': plh_cfg.runtime_dir = optarg; break;
        case 'b': plh_cfg.cap_backend = optarg; break;
        default: usage(); return o == 'h' ? 0 : 2;
        }
    }
    if (!plh_cfg.runtime_dir) {
        const char *x = getenv("XDG_RUNTIME_DIR");
        if (!x) { PLH_ERR("bridge", "XDG_RUNTIME_DIR not set"); return 1; }
        snprintf(rdir, sizeof rdir, "%s/%s", x, PLH_RUNTIME_SUBDIR);
        plh_cfg.runtime_dir = rdir;
    }
    if (mkdir(plh_cfg.runtime_dir, 0700) < 0 && errno != EEXIST) { PLH_ERR("bridge", "mkdir %s: %m", plh_cfg.runtime_dir); return 1; }
    struct stat st;
    if (lstat(plh_cfg.runtime_dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        PLH_ERR("bridge", "%s must be a 0700 directory owned by the user", plh_cfg.runtime_dir);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    epfd = epoll_create1(EPOLL_CLOEXEC);

    /* single instance: if the socket answers, a daemon is already running */
    char sock_path[300];
    snprintf(sock_path, sizeof sock_path, "%s/%s", plh_cfg.runtime_dir, PLH_SOCKET_NAME);
    struct sockaddr_un ua = { .sun_family = AF_UNIX };
    if (strlen(sock_path) >= sizeof ua.sun_path) { PLH_ERR("bridge", "path too long"); return 1; }
    strcpy(ua.sun_path, sock_path);
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(probe, (struct sockaddr *)&ua, sizeof ua) == 0) {
        PLH_ERR("bridge", "a plh-bridge is already listening on %s", sock_path);
        return 1;
    }
    close(probe);
    unlink(sock_path);
    int ufd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (bind(ufd, (struct sockaddr *)&ua, sizeof ua) < 0 || listen(ufd, SOMAXCONN) < 0) {
        PLH_ERR("bridge", "listen %s: %m", sock_path);
        return 1;
    }
    chmod(sock_path, 0600);

    int tfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(tfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in ta = { .sin_family = AF_INET, .sin_port = htons((uint16_t)plh_cfg.tcp_port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t tl = sizeof ta;
    if (bind(tfd, (struct sockaddr *)&ta, sizeof ta) < 0 || listen(tfd, SOMAXCONN) < 0 ||
        getsockname(tfd, (struct sockaddr *)&ta, &tl) < 0) {
        PLH_ERR("bridge", "TCP listen: %m");
        return 1;
    }

    if (getrandom(token, sizeof token, 0) != sizeof token) { PLH_ERR("bridge", "getrandom: %m"); return 1; }
    char portbuf[16];
    int pl = snprintf(portbuf, sizeof portbuf, "%u\n", ntohs(ta.sin_port));
    if (write_file(plh_cfg.runtime_dir, PLH_TOKEN_FILE, token, sizeof token) < 0 ||
        write_file(plh_cfg.runtime_dir, PLH_PORT_FILE, portbuf, (size_t)pl) < 0) {
        PLH_ERR("bridge", "writing port/token: %m");
        return 1;
    }

    for (int i = 1; i < PLH_SVC_COUNT; i++) {
        if (!services[i]) continue;
        service_ok[i] = !services[i]->init || services[i]->init() == 0;
        PLH_INFO("bridge", "service %s: %s", services[i]->name, service_ok[i] ? "active" : "UNAVAILABLE");
    }

    reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    plh_timer_add(500000000ull, 500000000ull, auth_timeout_cb, NULL);
    plh_loop_add_fd(ufd, EPOLLIN, accept_cb, (void *)1);
    plh_loop_add_fd(tfd, EPOLLIN, accept_cb, NULL);
    PLH_INFO("bridge", "ready: unix %s, tcp 127.0.0.1:%u, protocol %d.%d", sock_path, ntohs(ta.sin_port),
             PLH_PROTO_VERSION_MAJOR, PLH_PROTO_VERSION_MINOR);
    fflush(stderr);

    while (!quit) {
        struct epoll_event evs[64];
        int n = epoll_wait(epfd, evs, 64, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            struct watch *w = (fd >= 0 && fd < MAX_WATCH) ? watches[fd] : NULL;
            if (w && w->cb) w->cb(fd, evs[i].events, w->data);
            else if (w && w->tcb) timer_fire(fd, evs[i].events, w->data);
        }
        reap_closed();
    }

    PLH_INFO("bridge", "stopping");
    while (clients) client_close(clients);
    reap_closed();
    for (int i = PLH_SVC_COUNT - 1; i > 0; i--)
        if (service_ok[i] && services[i]->fini) services[i]->fini();
    unlink(sock_path);
    char p[512];
    snprintf(p, sizeof p, "%s/%s", plh_cfg.runtime_dir, PLH_PORT_FILE); unlink(p);
    snprintf(p, sizeof p, "%s/%s", plh_cfg.runtime_dir, PLH_TOKEN_FILE); unlink(p);
    return 0;
}
