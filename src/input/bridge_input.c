/* bridge_input.c — service 0x03 "input" of the plh-bridge daemon.
 * Receives Windows events (plh-input-hook.exe) and replays them on the real desktop through a
 * plh_input backend (uinput today; libei/RemoteDesktop portal not wired yet).
 * Safety: each client has its own state (held keys/buttons), released when it disconnects. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <linux/input-event-codes.h>
#include "plh_bridge.h"
#include "proto/plh_proto_input.h"  /* until plh_proto.h includes it */
#include "plh_input.h"

#define MOD "input"

static plh_input *in;
static struct plh_output outs[16];
static int nouts;
static plh_rect desk;
static int def_tx, def_ty;        /* default translation (primary KWin output) */
/* Mouse mode for motion coming from the hook (which sends ABS + inferred REL):
 * 0 = absolute (desktop, default), 1 = relative (pointer-locked games). PLH_INPUT_MOUSE_MODE=abs|rel.
 * Not implemented: automatic mode based on host cursor visibility (capture cursor metadata). */
static int mouse_rel_mode;

#define MAX_REDIR 4

struct icli {
    struct plh_redir redir[MAX_REDIR];   /* Windows rect -> KWin rect, released with the client */
    int nredir;
    struct icli *next;            /* all clients, to shift tx/ty when the KWin origin moves */
    int tx, ty;                   /* KWin = Windows + T, for this client */
    unsigned char keys[KEY_CNT / 8 + 1];
    unsigned char btns;
    plh_pad *pads[4];             /* virtual gamepads owned by this client */
};

static int nclients;
static struct icli *clients;
static int refresh_timer = -1;

/* Re-read the KWin layout: desktop rect (virtual output included), default translation, uinput
 * range. Called at start, on SET_LAYOUT, periodically while clients exist, and before rejecting
 * a point outside the cached desktop. Returns 1 if the desktop rect changed. */
static int refresh_layout(void)
{
    struct plh_output o[16];
    const char *src;
    int n = plh_detect_outputs(o, 16, &src);
    if (n <= 0) return 0;     /* keep the last known layout */
    plh_rect d; int tx, ty;
    if (plh_layout_snapshot(o, n, &d, &tx, &ty)) return 0;
    memcpy(outs, o, sizeof o); nouts = n;
    if (tx != def_tx || ty != def_ty) {
        /* KWin moved the primary output: every client's Windows primary stays on the same output,
         * so its translation moves by the same delta. */
        for (struct icli *c = clients; c; c = c->next) plh_translation_shift(&c->tx, &c->ty, def_tx, def_ty, tx, ty);
        PLH_INFO(MOD, "default translation %+d,%+d -> %+d,%+d (client translations shifted)", def_tx, def_ty, tx, ty);
    }
    def_tx = tx; def_ty = ty;
    if (!memcmp(&d, &desk, sizeof d)) return 0;
    PLH_INFO(MOD, "desktop %d,%d %dx%d -> %d,%d %dx%d", desk.x, desk.y, desk.w, desk.h, d.x, d.y, d.w, d.h);
    desk = d;
    if (in) {
        int r = plh_uinput_resize(in, &d);
        if (r == 1) PLH_INFO(MOD, "uinput resize deferred (button down)");
        else if (r < 0) PLH_ERR(MOD, "uinput resize failed: %d", r);
        else PLH_INFO(MOD, "uinput absolute device recreated in %ld us", plh_uinput_last_resize_us(in));
    }
    return 1;
}

static void refresh_tick(void *data) { (void)data; if (nclients > 0) refresh_layout(); }

static struct icli *cli_state(struct plh_client *c)
{
    struct icli *s = plh_client_get_data(c, PLH_SVC_INPUT);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) return NULL;
        if (nclients == 0) refresh_layout();
        nclients++;
        s->next = clients; clients = s;
        s->tx = def_tx; s->ty = def_ty;
        plh_client_set_data(c, PLH_SVC_INPUT, s);
    }
    return s;
}

static void release_all(struct icli *s)
{
    for (int k = 0; k < KEY_CNT; k++)
        if (s->keys[k / 8] & (1u << (k % 8))) plh_input_key_evdev(in, k, 0);
    for (int b = 0; b < PLH_BTN_COUNT; b++)
        if (s->btns & (1u << b)) plh_input_button(in, (enum plh_button)b, 0);
    memset(s->keys, 0, sizeof s->keys);
    s->btns = 0;
}

static void drop_pads(struct icli *s)
{
    for (int i = 0; i < 4; i++) { plh_pad_destroy(s->pads[i]); s->pads[i] = NULL; }
}

static int apply(struct icli *s, const struct plh_input_ev *e)
{
    switch (e->type) {
    case PLH_IEV_KEY: {
        int code = 0;
        if (e->code) code = plh_scancode_to_evdev(e->code, e->flags & PLH_IEV_F_EXTENDED);
        if (!code && e->a) code = plh_vk_to_evdev((unsigned)e->a, e->flags & PLH_IEV_F_EXTENDED);
        if (!code || code >= KEY_CNT) return -1;
        int down = e->b != 0;
        int held = (s->keys[code / 8] >> (code % 8)) & 1;
        if (!down && !held) return 0;   /* orphan release: ignore (no side effect) */
        if (down) s->keys[code / 8] |= (unsigned char)(1u << (code % 8));
        else s->keys[code / 8] &= (unsigned char)~(1u << (code % 8));
        return plh_input_key_evdev(in, code, down);
    }
    case PLH_IEV_MOVE_REL:
        if ((e->flags & PLH_IEV_F_INFERRED) && !mouse_rel_mode) return 1;   /* dropped (absolute mode) */
        return plh_input_move_rel(in, e->a, e->b);
    case PLH_IEV_MOVE_ABS:
        if (mouse_rel_mode) return 1;                                       /* dropped (relative mode) */
        {   /* outside the desktop = invalid message: rejected (never clamped to an edge, see the 0,31 incident) */
            int x = e->a + s->tx, y = e->b + s->ty;
            plh_redirect_map(s->redir, s->nredir, e->a, e->b, &x, &y);   /* before the in-desktop check */
            if (x < desk.x || y < desk.y || x >= desk.x + desk.w || y >= desk.y + desk.h)
                refresh_layout();   /* cached desktop may be stale (screen added/removed); re-checked below */
            if (x < desk.x || y < desk.y || x >= desk.x + desk.w || y >= desk.y + desk.h) return -1;
            return plh_input_move_abs(in, x, y);
        }
    case PLH_IEV_BUTTON:
        if (e->code >= PLH_BTN_COUNT) return -1;
        if (e->b) s->btns |= (unsigned char)(1u << e->code);
        else if (!(s->btns & (1u << e->code))) return 0;
        else s->btns &= (unsigned char)~(1u << e->code);
        return plh_input_button(in, (enum plh_button)e->code, e->b != 0);
    case PLH_IEV_WHEEL:
        return plh_input_wheel(in, e->a, e->b);
    }
    return -1;
}

static void handle(struct plh_client *c, const struct plh_msg_hdr *h, const void *p)
{
    /* Only clients declared "input" (plh-input-hook) or native tools may inject:
     * an adversarial bridge test or another service must never move the real cursor. */
    uint32_t kind = plh_client_kind(c);
    if (kind != PLH_CLIENT_INPUT && !(kind == PLH_CLIENT_TOOL && plh_client_is_native(c))) {
        PLH_WARN(MOD, "client %u (type %u) refused: only INPUT clients inject", plh_client_id(c), kind);
        if (h->seq) plh_reply_error(c, h, PLH_E_PERM, "client not allowed to inject");
        return;
    }
    struct icli *s = cli_state(c);
    if (!s) { if (h->seq) plh_reply_error(c, h, PLH_E_NOMEM, "out of memory"); return; }
    switch (h->type) {
    case PLH_MSG_INPUT_EVENTS: {
        const struct plh_input_events_hdr *eh = p;
        if (h->size < sizeof *eh || eh->count > 4096 ||
            h->size < sizeof *eh + (uint64_t)eh->count * sizeof(struct plh_input_ev)) {
            if (h->seq) plh_reply_error(c, h, PLH_E_PROTO, "size");
            return;
        }
        const struct plh_input_ev *ev = (const void *)(eh + 1);
        struct plh_input_status st = { PLH_OK, 0 };
        for (uint32_t i = 0; i < eh->count; i++) {
            int r = apply(s, &ev[i]);
            if (r == 0) st.applied++;
            else if (r < 0) PLH_DBG(MOD, "event ignored type=%u code=0x%x a=%d b=%d", ev[i].type, ev[i].code, ev[i].a, ev[i].b);
        }
        if (h->seq) plh_reply(c, h, &st, sizeof st);
        return;
    }
    case PLH_MSG_INPUT_SET_LAYOUT: {
        const struct plh_input_layout_hdr *lh = p;
        if (h->size < sizeof *lh || lh->count == 0 || lh->count > 16 ||
            h->size < sizeof *lh + lh->count * sizeof(struct plh_input_monitor)) {
            if (h->seq) plh_reply_error(c, h, PLH_E_PROTO, "size");
            return;
        }
        const struct plh_input_monitor *m = (const void *)(lh + 1);
        plh_rect w[16]; int prim = 0;
        for (uint32_t i = 0; i < lh->count; i++) {
            w[i] = (plh_rect){ m[i].x, m[i].y, m[i].w, m[i].h };
            if (m[i].primary) prim = (int)i;
        }
        refresh_layout();   /* the layout may have changed (screen unplugged, virtual output) */
        struct plh_input_layout_reply r = { PLH_OK, def_tx, def_ty, desk.x, desk.y, desk.w, desk.h, 1 };
        if (nouts > 0 && plh_layout_translation(w, (int)lh->count, prim, outs, nouts, &r.tx, &r.ty) == 0) {
            s->tx = r.tx; s->ty = r.ty;
            PLH_INFO(MOD, "client %u: Windows->KWin translation %+d,%+d", plh_client_id(c), r.tx, r.ty);
        } else {
            r.status = PLH_E_NOENT;
            PLH_WARN(MOD, "client %u: Windows layout incompatible with KWin, default translation %+d,%+d",
                     plh_client_id(c), def_tx, def_ty);
        }
        if (h->seq) plh_reply(c, h, &r, sizeof r);
        return;
    }
    case PLH_MSG_INPUT_GAMEPAD: {
        const struct plh_input_gamepad *g = p;
        if (h->size < sizeof *g || g->index >= 4) { if (h->seq) plh_reply_error(c, h, PLH_E_PROTO, "size"); return; }
        if (!g->connected) { plh_pad_destroy(s->pads[g->index]); s->pads[g->index] = NULL; }
        else {
            if (!s->pads[g->index]) {
                char err[128];
                s->pads[g->index] = plh_pad_create((int)g->index, err, sizeof err);
                if (!s->pads[g->index]) { PLH_ERR(MOD, "gamepad %u: %s", g->index, err); if (h->seq) plh_reply_error(c, h, PLH_E_IO, "gamepad"); return; }
                PLH_INFO(MOD, "client %u: virtual gamepad %u plugged", plh_client_id(c), g->index);
            }
            plh_pad_update(s->pads[g->index], g->buttons, g->left_trigger, g->right_trigger, g->lx, g->ly, g->rx, g->ry);
        }
        if (h->seq) { struct plh_input_status st = { PLH_OK, 1 }; plh_reply(c, h, &st, sizeof st); }
        return;
    }
    case PLH_MSG_INPUT_REDIRECT: {
        const struct plh_input_redirect_hdr *rh = p;
        if (h->size < sizeof *rh || rh->count > MAX_REDIR ||
            h->size < sizeof *rh + (uint64_t)rh->count * sizeof(struct plh_redirect)) {
            if (h->seq) plh_reply_error(c, h, PLH_E_PROTO, "size");
            return;
        }
        const struct plh_redirect *pr = (const void *)(rh + 1);
        s->nredir = 0;                                   /* the new table replaces the old one */
        for (uint32_t i = 0; i < rh->count; i++) {
            struct plh_redir e = {
                { pr[i].from.left, pr[i].from.top, pr[i].from.right - pr[i].from.left, pr[i].from.bottom - pr[i].from.top },
                { pr[i].to.left, pr[i].to.top, pr[i].to.right - pr[i].to.left, pr[i].to.bottom - pr[i].to.top } };
            if (e.from.w <= 0 || e.from.h <= 0 || e.to.w <= 0 || e.to.h <= 0) continue;   /* degenerate: ignored */
            s->redir[s->nredir++] = e;
        }
        PLH_INFO(MOD, "client %u: %d redirect entries", plh_client_id(c), s->nredir);
        if (h->seq) plh_reply(c, h, NULL, 0);
        return;
    }
    case PLH_MSG_INPUT_RELEASE_ALL: {
        release_all(s);
        struct plh_input_status st = { PLH_OK, 0 };
        if (h->seq) plh_reply(c, h, &st, sizeof st);
        return;
    }
    }
    if (h->seq) plh_reply_error(c, h, PLH_E_UNKNOWN, "unknown input message 0x%04x", h->type);
}

static void client_gone(struct plh_client *c)
{
    struct icli *s = plh_client_get_data(c, PLH_SVC_INPUT);
    if (!s) return;
    release_all(s);
    drop_pads(s);
    for (struct icli **pp = &clients; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    free(s);   /* redirect table goes with it */
    plh_client_set_data(c, PLH_SVC_INPUT, NULL);
    if (nclients > 0) nclients--;
}

static int init(void)
{
    /* Opt-in: the service exists (and creates devices) only if PLH_INPUT_BACKEND=uinput.
     * Daemons started for capture/encode/adversarial tests therefore never inject. */
    const char *be = getenv("PLH_INPUT_BACKEND");
    if (!be || strcmp(be, "uinput")) { PLH_INFO(MOD, "disabled (set PLH_INPUT_BACKEND=uinput to enable)"); return -1; }
    const char *mm = getenv("PLH_INPUT_MOUSE_MODE");
    mouse_rel_mode = mm && !strcmp(mm, "rel");
    const char *osrc;
    nouts = plh_detect_outputs(outs, 16, &osrc);
    PLH_INFO(MOD, "screen layout read via %s", osrc);
    if (nouts > 0) {
        plh_layout_snapshot(outs, nouts, &desk, &def_tx, &def_ty);
    } else {
        desk = (plh_rect){ 0, 0, 5760, 1080 };
        PLH_WARN(MOD, "no way to read the screen layout: assuming desktop 0,0 5760x1080");
    }
    char err[256];
    in = plh_uinput_open(&desk, err, sizeof err);
    if (!in) { PLH_ERR(MOD, "uinput: %s", err); return -1; }
    char k[16], r[16], a[16];
    if (plh_uinput_evnodes(in, k, r, a) == 0) {
        if (plh_kwin_set_flat_accel(r, 3000, err, sizeof err))
            PLH_WARN(MOD, "flat acceleration not set: %s", err);
        PLH_INFO(MOD, "uinput: keyboard %s, mouse %s, absolute %s; desktop %d,%d %dx%d; Windows->KWin %+d,%+d; mouse %s",
                 k, r, a, desk.x, desk.y, desk.w, desk.h, def_tx, def_ty, mouse_rel_mode ? "relative" : "absolute");
    }
    refresh_timer = plh_timer_add(2000000000ull, 2000000000ull, refresh_tick, NULL);
    return 0;
}

static void fini(void)
{
    if (refresh_timer >= 0) plh_timer_del(refresh_timer);
    refresh_timer = -1;
    plh_input_close(in);
    in = NULL;
}

static const struct plh_service input_service = {
    .name = "input", .id = PLH_SVC_INPUT, .init = init, .fini = fini,
    .handle = handle, .client_gone = client_gone,
};
PLH_SERVICE_REGISTER(input_service)
