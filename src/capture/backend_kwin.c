/* backend_kwin.c — direct capture via zkde_screencast_unstable_v1 (KWin), no dialog.
 * Owner: capture. KWin creates one PipeWire node per output; it is consumed with pw_consumer.
 * Prerequisite: KWin must advertise zkde_screencast_unstable_v1 to our client (checked at run time). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <wayland-client.h>
#include "zkde-screencast-unstable-v1-client-protocol.h"
#include "pw_consumer.h"

#define MOD "kwin"

struct kout {
    struct wl_output *wl;
    uint32_t global;
    char name[PLH_NAME_MAX];
};

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct zkde_screencast_unstable_v1 *sc;
static uint32_t sc_version;
static struct kout outs[CAP_MAX_OUTPUTS];
static int nouts;

struct kwin_be {
    struct cap_stream *s;
    struct zkde_screencast_stream_unstable_v1 *st;
    struct cap_pw *pw;
    uint32_t node;
};

static void o_geometry(void *d, struct wl_output *w, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
                       const char *make, const char *model, int32_t tr)
{ (void)d; (void)w; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr; }
static void o_mode(void *d, struct wl_output *w, uint32_t f, int32_t a, int32_t b, int32_t c)
{ (void)d; (void)w; (void)f; (void)a; (void)b; (void)c; }
static void o_done(void *d, struct wl_output *w) { (void)d; (void)w; }
static void o_scale(void *d, struct wl_output *w, int32_t f) { (void)d; (void)w; (void)f; }
static void o_name(void *d, struct wl_output *w, const char *n) { (void)w; snprintf(((struct kout *)d)->name, PLH_NAME_MAX, "%s", n); }
static void o_desc(void *d, struct wl_output *w, const char *n) { (void)d; (void)w; (void)n; }
static const struct wl_output_listener out_l = { o_geometry, o_mode, o_done, o_scale, o_name, o_desc };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, zkde_screencast_unstable_v1_interface.name)) {
        sc_version = ver < 6 ? ver : 6;
        sc = wl_registry_bind(r, name, &zkde_screencast_unstable_v1_interface, sc_version);
    } else if (!strcmp(iface, wl_output_interface.name) && nouts < CAP_MAX_OUTPUTS && ver >= 4) {
        struct kout *o = &outs[nouts++];
        memset(o, 0, sizeof *o);
        o->global = name;
        o->wl = wl_registry_bind(r, name, &wl_output_interface, 4);
        wl_output_add_listener(o->wl, &out_l, o);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r;
    for (int i = 0; i < nouts; i++)
        if (outs[i].global == name) {
            wl_output_release(outs[i].wl);
            outs[i] = outs[--nouts];
            break;
        }
}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void wl_io(int fd, uint32_t ev, void *d)
{
    (void)fd; (void)d;
    if (ev & (EPOLLERR | EPOLLHUP)) {
        PLH_ERR(MOD, "Wayland connection lost");
        plh_loop_del_fd(fd);
        return;
    }
    if (wl_display_dispatch(dpy) < 0) {
        PLH_ERR(MOD, "wl_display_dispatch failed");
        plh_loop_del_fd(fd);
        return;
    }
    wl_display_flush(dpy);
}

static int ensure_wl(void)
{
    if (dpy) return sc ? 0 : PLH_E_PERM;
    dpy = wl_display_connect(NULL);
    if (!dpy) return PLH_E_IO;
    reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    plh_loop_add_fd(wl_display_get_fd(dpy), EPOLLIN, wl_io, NULL);
    if (!sc) {
        PLH_WARN(MOD, "zkde_screencast_unstable_v1 not advertised by the compositor (not authorized or not KWin)");
        return PLH_E_PERM;
    }
    PLH_INFO(MOD, "zkde_screencast_unstable_v1 v%u bound, %d outputs", sc_version, nouts);
    return 0;
}

static void st_closed(void *d, struct zkde_screencast_stream_unstable_v1 *st)
{
    (void)st;
    struct kwin_be *k = d;
    cap_stream_state(k->s, PLH_CAP_STATE_CLOSED, 0, "KWin closed the stream");
}
static void start_pw(struct kwin_be *k, const char *target, uint32_t node)
{
    if (k->pw) return;
    k->pw = cap_pw_start(k->s, -1, target, node);
    if (!k->pw) cap_stream_state(k->s, PLH_CAP_STATE_ERROR, PLH_E_IO, "PipeWire");
}
static void st_created(void *d, struct zkde_screencast_stream_unstable_v1 *st, uint32_t node)
{
    (void)st;
    struct kwin_be *k = d;
    k->node = node;
    PLH_INFO(MOD, "stream %u: PipeWire node %u created by KWin", k->s->id, node);
    if (sc_version < 6) {
        char t[16];
        snprintf(t, sizeof t, "%u", node);
        start_pw(k, t, node);
    }
}
static void st_failed(void *d, struct zkde_screencast_stream_unstable_v1 *st, const char *err)
{
    (void)st;
    struct kwin_be *k = d;
    PLH_ERR(MOD, "stream %u: KWin refuses: %s", k->s->id, err);
    cap_stream_state(k->s, PLH_CAP_STATE_ERROR, PLH_E_PERM, err);
}
static void st_serial(void *d, struct zkde_screencast_stream_unstable_v1 *st, uint32_t hi, uint32_t lo)
{
    (void)st;
    struct kwin_be *k = d;
    char t[32];
    snprintf(t, sizeof t, "%llu", ((unsigned long long)hi << 32) | lo);
    PLH_INFO(MOD, "stream %u: object.serial %s", k->s->id, t);
    start_pw(k, t, 0);
}
static const struct zkde_screencast_stream_unstable_v1_listener st_l = { st_closed, st_created, st_failed, st_serial };

static int kwin_start(struct cap_stream *s, const struct plh_cap_output *out)
{
    int r = ensure_wl();
    if (r) return r;
    struct kout *o = NULL;
    for (int i = 0; i < nouts; i++) if (!strcmp(outs[i].name, out->name)) o = &outs[i];
    if (!o) return PLH_E_NOENT;
    struct kwin_be *k = calloc(1, sizeof *k);
    if (!k) return PLH_E_NOMEM;
    k->s = s;
    s->be_data = k;
    uint32_t ptr = (s->flags & PLH_CAP_F_CURSOR_EMBEDDED) ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED
                                                          : ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA;
    k->st = zkde_screencast_unstable_v1_stream_output(sc, o->wl, ptr);
    zkde_screencast_stream_unstable_v1_add_listener(k->st, &st_l, k);
    wl_display_flush(dpy);
    return PLH_OK; /* the node arrives asynchronously */
}

static void kwin_stop(struct cap_stream *s)
{
    struct kwin_be *k = s->be_data;
    if (!k) return;
    cap_pw_stop(k->pw);
    if (k->st) zkde_screencast_stream_unstable_v1_close(k->st);
    if (dpy) wl_display_flush(dpy);
    free(k);
    s->be_data = NULL;
}

const struct cap_backend_ops cap_backend_kwin = { "kwin", PLH_CAP_BACKEND_KWIN, kwin_start, kwin_stop };
