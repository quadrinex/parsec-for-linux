/*
 * plh_bridge.h — internal API of the plh-bridge daemon for services (capture, encoding, input...).
 * Owner: capture. A service registers with PLH_SERVICE_REGISTER and is called
 * from the single event loop (single-threaded: no locking needed in the
 * callbacks; a service with its own threads wakes the loop via plh_loop_add_fd/eventfd).
 */
#ifndef PLH_BRIDGE_H
#define PLH_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "proto/plh_proto.h"

struct plh_client;

/* ---- journal ---- */
enum { PLH_LOG_ERR = 0, PLH_LOG_WARN = 1, PLH_LOG_INFO = 2, PLH_LOG_DEBUG = 3 };
void plh_log(int level, const char *mod, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define PLH_ERR(m, ...)  plh_log(PLH_LOG_ERR, m, __VA_ARGS__)
#define PLH_WARN(m, ...) plh_log(PLH_LOG_WARN, m, __VA_ARGS__)
#define PLH_INFO(m, ...) plh_log(PLH_LOG_INFO, m, __VA_ARGS__)
#define PLH_DBG(m, ...)  plh_log(PLH_LOG_DEBUG, m, __VA_ARGS__)

uint64_t plh_now_ns(void); /* CLOCK_MONOTONIC */

/* ---- event loop ---- */
typedef void (*plh_fd_cb)(int fd, uint32_t events, void *data);
int  plh_loop_add_fd(int fd, uint32_t epoll_events, plh_fd_cb cb, void *data);
int  plh_loop_mod_fd(int fd, uint32_t epoll_events);
void plh_loop_del_fd(int fd);
/* periodic timer (timerfd); returns the fd (to pass to plh_timer_del) or -1 */
typedef void (*plh_timer_cb)(void *data);
int  plh_timer_add(uint64_t first_ns, uint64_t period_ns, plh_timer_cb cb, void *data);
void plh_timer_del(int tfd);

/* ---- clients ---- */
uint32_t plh_client_id(const struct plh_client *c);
uint32_t plh_client_kind(const struct plh_client *c);
int      plh_client_is_native(const struct plh_client *c); /* AF_UNIX: can receive fds */
/* Private data per service and per client (NULL at the start). */
void   *plh_client_get_data(struct plh_client *c, int svc);
void    plh_client_set_data(struct plh_client *c, int svc, void *data);

/* Send (copied into the output buffer, non-blocking). fds/nfds: native clients only. */
int plh_send(struct plh_client *c, uint16_t type, uint16_t flags, uint32_t seq,
             const void *payload, uint32_t size, const int *fds, int nfds);
/* Send in two pieces (payload header + array) with no intermediate copy on the caller side. */
int plh_send2(struct plh_client *c, uint16_t type, uint16_t flags, uint32_t seq,
              const void *p1, uint32_t s1, const void *p2, uint32_t s2);
int plh_reply(struct plh_client *c, const struct plh_msg_hdr *req, const void *payload, uint32_t size);
int plh_reply_error(struct plh_client *c, const struct plh_msg_hdr *req, int32_t status, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
int plh_send_event(struct plh_client *c, uint16_t type, const void *payload, uint32_t size);

/* ---- shared memory (/dev/shm/plh-<uid>-<tag>-<n>, 0600) ---- */
struct plh_shm {
    int      fd;
    void    *map;
    uint64_t size;
    char     name[PLH_NAME_MAX]; /* without "/dev/shm/" */
};
int  plh_shm_create(struct plh_shm *shm, const char *tag, uint64_t size);
void plh_shm_destroy(struct plh_shm *shm); /* munmap + close + unlink */

/* ---- services ---- */
struct plh_service {
    const char *name;
    int         id;                             /* enum plh_service_id */
    int  (*init)(void);                         /* 0 = OK; <0: service disabled */
    void (*fini)(void);
    /* message for this service; payload is valid during the call only */
    void (*handle)(struct plh_client *c, const struct plh_msg_hdr *h, const void *payload);
    void (*client_gone)(struct plh_client *c);  /* free the client's resources */
};
void plh_service_register(const struct plh_service *svc);
#define PLH_SERVICE_REGISTER(var) \
    __attribute__((constructor)) static void plh_reg_##var(void) { plh_service_register(&(var)); }

/* configuration read by main (environment variables / options) */
struct plh_config {
    const char *runtime_dir;   /* $XDG_RUNTIME_DIR/plh-bridge */
    int         tcp_port;      /* 0 = ephemeral */
    int         verbose;
    const char *cap_backend;   /* "auto", "portal", "kwin", "test" */
};
extern struct plh_config plh_cfg;

#endif
