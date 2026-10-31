/* backend_portal.c — capture via xdg-desktop-portal ScreenCast (+ PipeWire), persist_mode=2.
 * Owner: capture. *** NOT TESTED on this machine: compiled only, no approval dialog
 * has been triggered. *** Serves GNOME, wlroots (xdg-desktop-portal-wlr), KDE as fallback.
 * Per stream: CreateSession → SelectSources(monitor, cursor as metadata, persist_mode=2, token)
 * → Start (dialog the first time) → OpenPipeWireRemote → PipeWire consumer.
 * Restore token: ~/.config/parsec-linux-host/portal-<output>.token (0600).
 * D-Bus through sd-bus (libsystemd), integrated into the daemon loop. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include "pw_consumer.h"

#define MOD "portal"
#define PD "org.freedesktop.portal.Desktop"
#define PO "/org/freedesktop/portal/desktop"
#define SC "org.freedesktop.portal.ScreenCast"

enum step { ST_CREATE, ST_SELECT, ST_START, ST_DONE };

struct portal_be {
    struct cap_stream *s;
    enum step step;
    sd_bus_slot *resp;           /* Response signal of the request in progress */
    char  token[40];
    char  session[256];
    struct cap_pw *pw;
    int   pwfd;
    int   dead;
};

static sd_bus *bus;
static int bus_fd = -1;
static unsigned counter;

static void bus_io(int fd, uint32_t ev, void *d);
static void bus_rearm(void)
{
    if (bus_fd >= 0) plh_loop_mod_fd(bus_fd, (uint32_t)sd_bus_get_events(bus) | EPOLLIN);
}
static void bus_io(int fd, uint32_t ev, void *d)
{
    (void)fd; (void)ev; (void)d;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) PLH_ERR(MOD, "sd_bus_process : %s", strerror(-r));
    bus_rearm();
}
static int bus_get(void)
{
    if (bus) return 0;
    if (sd_bus_open_user(&bus) < 0) { bus = NULL; return PLH_E_IO; }
    bus_fd = sd_bus_get_fd(bus);
    plh_loop_add_fd(bus_fd, EPOLLIN, bus_io, NULL);
    return 0;
}

/* ---- D-Bus helpers ---- */
static void make_token(struct portal_be *b, char *t, size_t n)
{
    snprintf(t, n, "plh%d_%u_%u", (int)getpid(), b->s->id, ++counter);
}
static int add_str(sd_bus_message *m, const char *k, const char *v)
{
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) return r;
    sd_bus_message_append(m, "s", k);
    sd_bus_message_append(m, "v", "s", v);
    return sd_bus_message_close_container(m);
}
static int add_u32(sd_bus_message *m, const char *k, uint32_t v)
{
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) return r;
    sd_bus_message_append(m, "s", k);
    sd_bus_message_append(m, "v", "u", v);
    return sd_bus_message_close_container(m);
}
static int add_bool(sd_bus_message *m, const char *k, int v)
{
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0) return r;
    sd_bus_message_append(m, "s", k);
    sd_bus_message_append(m, "v", "b", v);
    return sd_bus_message_close_container(m);
}

static void fail(struct portal_be *b, int status, const char *msg)
{
    PLH_ERR(MOD, "stream %u: %s", b->s->id, msg);
    b->dead = 1;
    cap_stream_state(b->s, PLH_CAP_STATE_ERROR, status, msg);
}

static int on_response(sd_bus_message *m, void *ud, sd_bus_error *err);

/* subscribes to the reply of the request whose token is `tok`, before calling the method */
static int watch(struct portal_be *b, const char *tok)
{
    char sender[128], path[512];
    const char *u = NULL;
    sd_bus_get_unique_name(bus, &u);
    snprintf(sender, sizeof sender, "%s", u ? u + 1 : "x");
    for (char *c = sender; *c; c++) if (*c == '.') *c = '_';
    snprintf(path, sizeof path, PO "/request/%s/%s", sender, tok);
    b->resp = sd_bus_slot_unref(b->resp);
    return sd_bus_match_signal(bus, &b->resp, PD, path, "org.freedesktop.portal.Request", "Response", on_response, b);
}

static int reply_check(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    struct portal_be *b = ud;
    (void)err;
    if (sd_bus_message_is_method_error(m, NULL)) {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        char msg[200];
        snprintf(msg, sizeof msg, "portal: %s", e && e->message ? e->message : "D-Bus error");
        fail(b, PLH_E_PERM, msg);
    }
    return 0;
}

static void do_create(struct portal_be *b)
{
    char tok[40], stok[40];
    make_token(b, tok, sizeof tok);
    make_token(b, stok, sizeof stok);
    sd_bus_message *m = NULL;
    watch(b, tok);
    sd_bus_message_new_method_call(bus, &m, PD, PO, SC, "CreateSession");
    sd_bus_message_open_container(m, 'a', "{sv}");
    add_str(m, "handle_token", tok);
    add_str(m, "session_handle_token", stok);
    sd_bus_message_close_container(m);
    sd_bus_call_async(bus, NULL, m, reply_check, b, 0);
    sd_bus_message_unref(m);
}

static void do_select(struct portal_be *b)
{
    char tok[40], rt[256] = "";
    make_token(b, tok, sizeof tok);
    sd_bus_message *m = NULL;
    watch(b, tok);
    sd_bus_message_new_method_call(bus, &m, PD, PO, SC, "SelectSources");
    sd_bus_message_append(m, "o", b->session);
    sd_bus_message_open_container(m, 'a', "{sv}");
    add_str(m, "handle_token", tok);
    add_u32(m, "types", 1);  /* MONITOR */
    add_bool(m, "multiple", 0);
    add_u32(m, "cursor_mode", (b->s->flags & PLH_CAP_F_CURSOR_EMBEDDED) ? 2 : 4); /* embedded / metadata */
    add_u32(m, "persist_mode", 2); /* until revoked */
    if (cap_token_load(b->s->out.name, rt, sizeof rt)) add_str(m, "restore_token", rt);
    sd_bus_message_close_container(m);
    sd_bus_call_async(bus, NULL, m, reply_check, b, 0);
    sd_bus_message_unref(m);
}

static void do_start(struct portal_be *b)
{
    char tok[40];
    make_token(b, tok, sizeof tok);
    sd_bus_message *m = NULL;
    watch(b, tok);
    sd_bus_message_new_method_call(bus, &m, PD, PO, SC, "Start");
    sd_bus_message_append(m, "os", b->session, "");
    sd_bus_message_open_container(m, 'a', "{sv}");
    add_str(m, "handle_token", tok);
    sd_bus_message_close_container(m);
    sd_bus_call_async(bus, NULL, m, reply_check, b, 0);
    sd_bus_message_unref(m);
}

static void do_open_pw(struct portal_be *b, uint32_t node)
{
    sd_bus_message *m = NULL, *rep = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message_new_method_call(bus, &m, PD, PO, SC, "OpenPipeWireRemote");
    sd_bus_message_append(m, "o", b->session);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_close_container(m);
    int r = sd_bus_call(bus, m, 5000000, &e, &rep); /* short and local: does not wait for the human */
    sd_bus_message_unref(m);
    int fd = -1;
    if (r >= 0 && sd_bus_message_read(rep, "h", &fd) >= 0 && fd >= 0) b->pwfd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    sd_bus_message_unref(rep);
    sd_bus_error_free(&e);
    if (b->pwfd < 0) { fail(b, PLH_E_IO, "OpenPipeWireRemote failed"); return; }
    char t[16];
    snprintf(t, sizeof t, "%u", node);
    b->pw = cap_pw_start(b->s, b->pwfd, t, node);
    if (!b->pw) fail(b, PLH_E_IO, "PipeWire (portal fd)");
    else PLH_INFO(MOD, "stream %u: PipeWire node %u via the portal", b->s->id, node);
}

static int on_response(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    struct portal_be *b = ud;
    (void)err;
    uint32_t code = 2;
    char session[256] = "", restore[256] = "";
    uint32_t node = 0;
    int have_node = 0;
    if (sd_bus_message_read(m, "u", &code) < 0) { fail(b, PLH_E_PROTO, "unreadable reply"); return 0; }
    if (code != 0) {
        fail(b, PLH_E_PERM, code == 1 ? "capture refused by the user" : "portal: request failed");
        return 0;
    }
    if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
        const char *k;
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            if (sd_bus_message_read(m, "s", &k) < 0) break;
            if (!strcmp(k, "session_handle") || !strcmp(k, "restore_token")) {
                const char *v = NULL;
                if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
                    sd_bus_message_read(m, "s", &v);
                    sd_bus_message_exit_container(m);
                }
                if (v) snprintf(!strcmp(k, "restore_token") ? restore : session,
                                !strcmp(k, "restore_token") ? sizeof restore : sizeof session, "%s", v);
            } else if (!strcmp(k, "streams")) {
                if (sd_bus_message_enter_container(m, 'v', "a(ua{sv})") > 0) {
                    if (sd_bus_message_enter_container(m, 'a', "(ua{sv})") > 0) {
                        if (sd_bus_message_enter_container(m, 'r', "ua{sv}") > 0) {
                            if (sd_bus_message_read(m, "u", &node) >= 0) have_node = 1;
                            sd_bus_message_skip(m, "a{sv}");
                            sd_bus_message_exit_container(m);
                        }
                        sd_bus_message_exit_container(m);
                    }
                    sd_bus_message_exit_container(m);
                }
            } else {
                sd_bus_message_skip(m, "v");
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    switch (b->step) {
    case ST_CREATE:
        if (!session[0]) { fail(b, PLH_E_PROTO, "no session_handle"); return 0; }
        snprintf(b->session, sizeof b->session, "%s", session);
        b->step = ST_SELECT;
        do_select(b);
        break;
    case ST_SELECT:
        b->step = ST_START;
        do_start(b); /* the approval dialog, if the token is missing or revoked, appears here */
        break;
    case ST_START:
        if (restore[0]) cap_token_save(b->s->out.name, restore);
        if (!have_node) { fail(b, PLH_E_NOENT, "the portal returned no stream"); return 0; }
        b->step = ST_DONE;
        do_open_pw(b, node);
        break;
    default: break;
    }
    bus_rearm();
    return 0;
}

static int portal_start(struct cap_stream *s, const struct plh_cap_output *out)
{
    (void)out;
    int r = bus_get();
    if (r) return r;
    struct portal_be *b = calloc(1, sizeof *b);
    if (!b) return PLH_E_NOMEM;
    b->s = s;
    b->pwfd = -1;
    s->be_data = b;
    do_create(b);
    bus_rearm();
    PLH_INFO(MOD, "stream %u: ScreenCast session requested for %s (approval may be pending)", s->id, out->name);
    return PLH_E_AGAIN;
}

static void portal_stop(struct cap_stream *s)
{
    struct portal_be *b = s->be_data;
    if (!b) return;
    cap_pw_stop(b->pw);
    if (b->session[0] && bus)
        sd_bus_call_method_async(bus, NULL, PD, b->session, "org.freedesktop.portal.Session", "Close", NULL, NULL, "");
    sd_bus_slot_unref(b->resp);
    if (b->pwfd >= 0) close(b->pwfd);
    if (bus) { bus_rearm(); sd_bus_flush(bus); }
    free(b);
    s->be_data = NULL;
}

const struct cap_backend_ops cap_backend_portal = { "portal", PLH_CAP_BACKEND_PORTAL, portal_start, portal_stop };
