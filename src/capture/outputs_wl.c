/* outputs_wl.c — enumerates the compositor outputs (wl_output v4 + xdg_output). Owner: capture.
 * Short Wayland connection on each call (a few ms): reflects layout changes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "cap_internal.h"

struct wo {
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    struct plh_cap_output o;
    int have_logical;
    int32_t gx, gy;
    int32_t transform;
    int32_t scale;
};

struct ctx {
    struct wl_registry *reg;
    struct zxdg_output_manager_v1 *xm;
    struct wo outs[CAP_MAX_OUTPUTS];
    int n;
};

/* stable ids per connector name during the daemon lifetime */
static char known[CAP_MAX_OUTPUTS * 4][PLH_NAME_MAX];
static int nknown;
uint32_t cap_stable_id(const char *name)
{
    for (int i = 0; i < nknown; i++) if (!strcmp(known[i], name)) return (uint32_t)i + 1;
    if (nknown >= (int)(sizeof known / sizeof known[0])) return 0;
    snprintf(known[nknown], PLH_NAME_MAX, "%s", name);
    return (uint32_t)++nknown;
}

static void o_geometry(void *d, struct wl_output *w, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub,
                       const char *make, const char *model, int32_t transform)
{
    (void)w; (void)pw; (void)ph; (void)sub;
    struct wo *o = d;
    o->gx = x; o->gy = y; o->transform = transform;
    if (!o->o.description[0]) snprintf(o->o.description, PLH_NAME_MAX, "%s %s", make, model);
}
static void o_mode(void *d, struct wl_output *w, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    (void)w;
    struct wo *o = d;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->o.width = (uint32_t)width; o->o.height = (uint32_t)height; o->o.refresh_mhz = (uint32_t)refresh;
    }
}
static void o_done(void *d, struct wl_output *w) { (void)d; (void)w; }
static void o_scale(void *d, struct wl_output *w, int32_t f) { (void)w; ((struct wo *)d)->scale = f; }
static void o_name(void *d, struct wl_output *w, const char *name)
{
    (void)w;
    snprintf(((struct wo *)d)->o.name, PLH_NAME_MAX, "%s", name);
}
static void o_desc(void *d, struct wl_output *w, const char *desc)
{
    (void)w;
    snprintf(((struct wo *)d)->o.description, PLH_NAME_MAX, "%s", desc);
}
static const struct wl_output_listener out_l = { o_geometry, o_mode, o_done, o_scale, o_name, o_desc };

static void x_pos(void *d, struct zxdg_output_v1 *x, int32_t px, int32_t py)
{
    (void)x;
    struct wo *o = d;
    o->o.x = px; o->o.y = py; o->have_logical = 1;
}
static void x_size(void *d, struct zxdg_output_v1 *x, int32_t w, int32_t h) { (void)d; (void)x; (void)w; (void)h; }
static void x_done(void *d, struct zxdg_output_v1 *x) { (void)d; (void)x; }
static void x_name(void *d, struct zxdg_output_v1 *x, const char *n)
{
    (void)x;
    struct wo *o = d;
    if (!o->o.name[0]) snprintf(o->o.name, PLH_NAME_MAX, "%s", n);
}
static void x_desc(void *d, struct zxdg_output_v1 *x, const char *n) { (void)d; (void)x; (void)n; }
static const struct zxdg_output_v1_listener xo_l = { x_pos, x_size, x_done, x_name, x_desc };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    struct ctx *c = d;
    if (!strcmp(iface, wl_output_interface.name) && c->n < CAP_MAX_OUTPUTS) {
        struct wo *o = &c->outs[c->n++];
        o->wl = wl_registry_bind(r, name, &wl_output_interface, ver < 4 ? ver : 4);
        wl_output_add_listener(o->wl, &out_l, o);
    } else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
        c->xm = wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, ver < 3 ? ver : 3);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static int cmp_out(const void *a, const void *b)
{
    const struct plh_cap_output *x = a, *y = b;
    if (x->y != y->y) return x->y < y->y ? -1 : 1;
    return x->x < y->x ? -1 : x->x > y->x;
}

int cap_outputs_query_wl(struct plh_cap_output *out, int max)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) return -1;
    struct ctx c;
    memset(&c, 0, sizeof c);
    c.reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(c.reg, &reg_l, &c);
    wl_display_roundtrip(dpy);
    if (c.xm)
        for (int i = 0; i < c.n; i++) {
            c.outs[i].xdg = zxdg_output_manager_v1_get_xdg_output(c.xm, c.outs[i].wl);
            zxdg_output_v1_add_listener(c.outs[i].xdg, &xo_l, &c.outs[i]);
        }
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    int n = 0;
    for (int i = 0; i < c.n && n < max; i++) {
        struct wo *o = &c.outs[i];
        if (!o->have_logical) { o->o.x = o->gx; o->o.y = o->gy; }
        o->o.scale_x1000 = (uint32_t)(o->scale > 0 ? o->scale * 1000 : 1000);
        static const uint32_t rot[] = { 0, 90, 180, 270, 0, 90, 180, 270 };
        o->o.rotation = rot[o->transform & 7];
        if (!o->o.name[0]) snprintf(o->o.name, PLH_NAME_MAX, "output-%d", i);
        o->o.output_id = cap_stable_id(o->o.name);
        if (o->o.x == 0 && o->o.y == 0) o->o.flags |= PLH_CAP_OUTPUT_PRIMARY;
        out[n++] = o->o;
        if (o->xdg) zxdg_output_v1_destroy(o->xdg);
        wl_output_release(o->wl);
    }
    if (c.xm) zxdg_output_manager_v1_destroy(c.xm);
    wl_registry_destroy(c.reg);
    wl_display_disconnect(dpy);
    qsort(out, (size_t)n, sizeof *out, cmp_out);
    return n;
}
