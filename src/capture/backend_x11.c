/* backend_x11.c — X11 capture: XShm (xcb-shm) + RandR 1.5 outputs + XFixes cursor.
 * Owner: capture. Periodic polling at max_fps (or monitor rate). CPU copy. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/randr.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include "cap_internal.h"

#define MOD "x11"

static xcb_connection_t *xc;
static xcb_screen_t *scr;

static int x_connect(void)
{
    if (xc && !xcb_connection_has_error(xc)) return 0;
    if (xc) { xcb_disconnect(xc); xc = NULL; }
    int sn;
    xc = xcb_connect(NULL, &sn);
    if (!xc || xcb_connection_has_error(xc)) { if (xc) xcb_disconnect(xc); xc = NULL; return PLH_E_IO; }
    const xcb_setup_t *su = xcb_get_setup(xc);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(su);
    for (; it.rem && sn > 0; sn--) xcb_screen_next(&it);
    scr = it.data;
    return scr ? 0 : PLH_E_IO;
}

int cap_outputs_query_x11(struct plh_cap_output *out, int max)
{
    int rc = x_connect();
    if (rc) return -1;
    int n = 0;
    xcb_randr_get_monitors_reply_t *mr =
        xcb_randr_get_monitors_reply(xc, xcb_randr_get_monitors(xc, scr->root, 1), NULL);
    if (mr) {
        xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(mr);
        for (; it.rem && n < max; xcb_randr_monitor_info_next(&it)) {
            xcb_randr_monitor_info_t *m = it.data;
            struct plh_cap_output *o = &out[n++];
            memset(o, 0, sizeof *o);
            o->x = m->x; o->y = m->y; o->width = m->width; o->height = m->height;
            o->refresh_mhz = 60000; o->scale_x1000 = 1000;
            if (m->primary) o->flags |= PLH_CAP_OUTPUT_PRIMARY;
            xcb_get_atom_name_reply_t *ar = xcb_get_atom_name_reply(xc, xcb_get_atom_name(xc, m->name), NULL);
            if (ar) {
                snprintf(o->name, sizeof o->name, "%.*s", xcb_get_atom_name_name_length(ar), xcb_get_atom_name_name(ar));
                free(ar);
            }
            if (!o->name[0]) snprintf(o->name, sizeof o->name, "monitor-%d", n - 1);
            o->output_id = cap_stable_id(o->name);
        }
        free(mr);
    }
    if (n == 0 && max > 0) { /* no RandR: a single screen = the root */
        struct plh_cap_output *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->width = scr->width_in_pixels; o->height = scr->height_in_pixels;
        o->refresh_mhz = 60000; o->scale_x1000 = 1000; o->flags = PLH_CAP_OUTPUT_PRIMARY;
        snprintf(o->name, sizeof o->name, "X11-screen");
        o->output_id = cap_stable_id(o->name);
    }
    return n;
}

struct x11_be {
    int      timer;
    xcb_shm_seg_t seg;
    int      shmid;
    uint8_t *buf;
    size_t   size;
    uint32_t cur_serial;
    int      cur_sent;
};

static void send_cursor(struct cap_stream *s, struct x11_be *b)
{
    xcb_xfixes_get_cursor_image_reply_t *c = xcb_xfixes_get_cursor_image_reply(xc, xcb_xfixes_get_cursor_image(xc), NULL);
    if (!c) return;
    int vis = c->x >= s->out.x && c->x < s->out.x + (int)s->width && c->y >= s->out.y && c->y < s->out.y + (int)s->height;
    int fresh = !b->cur_sent || c->cursor_serial != b->cur_serial;
    uint32_t w = c->width, h = c->height;
    if (w > s->cursor_max) w = s->cursor_max;
    if (h > s->cursor_max) h = s->cursor_max;
    uint32_t px[256 * 256];
    if (fresh) { /* XFixes: 32-bit premultiplied ARGB, stride c->width */
        const uint32_t *src = xcb_xfixes_get_cursor_image_cursor_image(c);
        for (uint32_t y = 0; y < h; y++) memcpy(px + y * w, src + (size_t)y * c->width, w * 4);
        b->cur_serial = c->cursor_serial; b->cur_sent = 1;
    }
    /* x,y = hotspot position relative to the output */
    cap_stream_cursor(s, vis, c->x - s->out.x, c->y - s->out.y, fresh ? (uint8_t *)px : NULL, w, h, w * 4,
                      c->xhot, c->yhot);
    free(c);
}

static void tick(void *d)
{
    struct cap_stream *s = d;
    struct x11_be *b = s->be_data;
    uint32_t slot;
    uint8_t *px = cap_stream_begin_frame(s, &slot);
    if (px) {
        xcb_shm_get_image_reply_t *r = xcb_shm_get_image_reply(
            xc, xcb_shm_get_image(xc, scr->root, (int16_t)s->out.x, (int16_t)s->out.y, (uint16_t)s->width,
                                  (uint16_t)s->height, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, b->seg, 0), NULL);
        if (!r) {
            cap_stream_drop_frame(s);
            cap_stream_state(s, PLH_CAP_STATE_ERROR, PLH_E_IO, "XShmGetImage failed");
            plh_timer_del(b->timer); b->timer = -1;
            return;
        }
        free(r);
        for (uint32_t y = 0; y < s->height; y++) {
            const uint32_t *src = (const uint32_t *)(b->buf + (size_t)y * s->width * 4);
            uint32_t *dst = (uint32_t *)(px + (size_t)y * s->stride);
            for (uint32_t x = 0; x < s->width; x++) dst[x] = src[x] | 0xff000000u;
        }
        cap_stream_commit_frame(s, slot, plh_now_ns(), 0);
    }
    send_cursor(s, b);
}

static int x11_start(struct cap_stream *s, const struct plh_cap_output *out)
{
    (void)out;
    int rc = x_connect();
    if (rc) return rc;
    if (scr->root_depth != 24 && scr->root_depth != 32) return PLH_E_UNKNOWN;
    xcb_shm_query_version_reply_t *v = xcb_shm_query_version_reply(xc, xcb_shm_query_version(xc), NULL);
    if (!v) return PLH_E_UNKNOWN;
    free(v);
    xcb_xfixes_query_version_reply_t *xv = xcb_xfixes_query_version_reply(xc, xcb_xfixes_query_version(xc, 4, 0), NULL);
    if (!xv) return PLH_E_UNKNOWN;
    free(xv);
    struct x11_be *b = calloc(1, sizeof *b);
    if (!b) return PLH_E_NOMEM;
    b->size = (size_t)s->width * s->height * 4;
    b->shmid = shmget(IPC_PRIVATE, b->size, IPC_CREAT | 0600);
    if (b->shmid < 0) { free(b); return PLH_E_NOMEM; }
    b->buf = shmat(b->shmid, NULL, 0);
    if (b->buf == (void *)-1) { shmctl(b->shmid, IPC_RMID, NULL); free(b); return PLH_E_NOMEM; }
    b->seg = xcb_generate_id(xc);
    xcb_void_cookie_t ck = xcb_shm_attach_checked(xc, b->seg, (uint32_t)b->shmid, 0);
    xcb_generic_error_t *e = xcb_request_check(xc, ck);
    shmctl(b->shmid, IPC_RMID, NULL); /* destroyed on last detach */
    if (e) { free(e); shmdt(b->buf); free(b); return PLH_E_UNKNOWN; }
    uint32_t fps = s->max_fps ? s->max_fps : 60;
    if (fps > 240) fps = 240;
    s->be_data = b;
    b->timer = plh_timer_add(1000000, 1000000000ull / fps, tick, s);
    if (b->timer < 0) {
        xcb_shm_detach(xc, b->seg); xcb_flush(xc); shmdt(b->buf); free(b); s->be_data = NULL;
        return PLH_E_IO;
    }
    cap_stream_state(s, PLH_CAP_STATE_STREAMING, 0, "X11 XShm");
    return PLH_OK;
}

static void x11_stop(struct cap_stream *s)
{
    struct x11_be *b = s->be_data;
    if (!b) return;
    if (b->timer >= 0) plh_timer_del(b->timer);
    xcb_shm_detach(xc, b->seg);
    xcb_flush(xc);
    shmdt(b->buf);
    free(b);
    s->be_data = NULL;
}

const struct cap_backend_ops cap_backend_x11 = { "x11", PLH_CAP_BACKEND_X11, x11_start, x11_stop };
