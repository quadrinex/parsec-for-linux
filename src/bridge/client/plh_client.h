/*
 * plh_client.h — bridge client library, builds natively (gcc) and as PE (mingw-w64).
 * Owner: capture. Compile plh_client.c into each DLL/tool (no dependency
 * beyond ws2_32 on the PE side, pthread on the native side).
 *
 * Connection: native -> AF_UNIX $XDG_RUNTIME_DIR/plh-bridge/bridge.sock;
 *             PE    -> TCP 127.0.0.1:<port> + token, read from Z:<XDG_RUNTIME_DIR>/plh-bridge/.
 *             Under Wine, XDG_RUNTIME_DIR is called WINE_HOST_XDG_RUNTIME_DIR (handled).
 *             PLH_BRIDGE_DIR (Unix path) replaces $XDG_RUNTIME_DIR/plh-bridge in both cases.
 *             PLH_BRIDGE_FORCE_TCP=1 forces TCP on native too (tests).
 *
 * Threads: all functions are thread-safe. Two receive modes:
 *  - without reader thread: plh_conn_request() reads the socket until its reply and passes the
 *    events it meets to the callback (called from the thread making the request);
 *  - plh_conn_start_reader(): an internal thread reads continuously, calls the callback for
 *    events and wakes up pending requests. (Recommended for dxgi.)
 */
#ifndef PLH_CLIENT_H
#define PLH_CLIENT_H

#include <stdint.h>
#include "../proto/plh_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

struct plh_conn;

/* Event callback (hdr.flags & PLH_F_EVENT, or a message with no pending request). */
typedef void (*plh_event_cb)(void *user, const struct plh_msg_hdr *h, const void *payload);

/* Returns PLH_OK or a negative enum plh_status. reply_out may be NULL. */
int  plh_conn_open(struct plh_conn **out, uint32_t client_kind, const char *name,
                   struct plh_hello_reply *reply_out);
void plh_conn_close(struct plh_conn *c);
void plh_conn_set_event_cb(struct plh_conn *c, plh_event_cb cb, void *user);
int  plh_conn_start_reader(struct plh_conn *c);
int  plh_conn_is_tcp(const struct plh_conn *c);

/* Synchronous request. The reply (payload) is copied into reply (at most reply_cap bytes),
 * *reply_len receives its real size. If the daemon replies PLH_MSG_ERROR, returns plh_error.status
 * (and copies the plh_error into reply if it fits). timeout_ms < 0: infinite. */
int  plh_conn_request(struct plh_conn *c, uint16_t type, const void *req, uint32_t req_len,
                      void *reply, uint32_t reply_cap, uint32_t *reply_len, int timeout_ms);
/* Send with no reply expected (seq = 0). */
int  plh_conn_send(struct plh_conn *c, uint16_t type, const void *payload, uint32_t len);
/* Without reader thread: handles incoming messages for at most timeout_ms (events ->
 * callback). Returns the number of messages handled or <0. */
int  plh_conn_poll(struct plh_conn *c, int timeout_ms);
/* Log in the daemon (PLH_MSG_LOG). */
void plh_conn_log(struct plh_conn *c, const char *fmt, ...);

/* Maps /dev/shm/<name> read/write (PE: via Z:\dev\shm\<name>). */
struct plh_shm_map {
    void    *ptr;
    uint64_t size;
    void    *priv[2];
};
int  plh_shm_map(struct plh_shm_map *m, const char *name, uint64_t size);
void plh_shm_unmap(struct plh_shm_map *m);

uint64_t plh_client_now_ns(void); /* PE: QueryPerformanceCounter; native: CLOCK_MONOTONIC */

#ifdef __cplusplus
}
#endif
#endif
