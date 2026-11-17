/* cap_service.c — capture service (0x01xx) of the plh-bridge daemon. Owner: capture. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cap_internal.h"

#define MOD "capture"

static struct cap_stream *streams;
static uint32_t next_stream_id = 1;

static const struct cap_backend_ops *backend_by_id(uint32_t id)
{
    switch (id) {
    case PLH_CAP_BACKEND_TEST: return &cap_backend_test;
    case PLH_CAP_BACKEND_PORTAL: return &cap_backend_portal;
    case PLH_CAP_BACKEND_KWIN: return &cap_backend_kwin;
    case PLH_CAP_BACKEND_X11: return &cap_backend_x11;
    default: return NULL;
    }
}

static uint32_t default_backend(void)
{
    const char *b = plh_cfg.cap_backend;
    if (!b || !strcmp(b, "auto")) return PLH_CAP_BACKEND_AUTO;
    if (!strcmp(b, "test")) return PLH_CAP_BACKEND_TEST;
    if (!strcmp(b, "portal")) return PLH_CAP_BACKEND_PORTAL;
    if (!strcmp(b, "kwin")) return PLH_CAP_BACKEND_KWIN;
    if (!strcmp(b, "x11")) return PLH_CAP_BACKEND_X11;
    return PLH_CAP_BACKEND_AUTO;
}

static struct cap_stream *find_stream(struct plh_client *c, uint32_t id)
{
    for (struct cap_stream *s = streams; s; s = s->next)
        if (s->id == id && s->client == c) return s;
    return NULL;
}

/* ---------------------------------------------------------------- API backends */
uint8_t *cap_stream_begin_frame(struct cap_stream *s, uint32_t *slot_out)
{
    s->captured++;
    for (uint32_t i = 0; i < s->slot_count; i++) {
        uint32_t k = (s->next_slot + i) % s->slot_count;
        if (!s->slot_held[k]) {
            s->writing = (int)k;
            *slot_out = k;
            return (uint8_t *)s->shm.map + s->slot_offset + (uint64_t)k * s->slot_size;
        }
    }
    s->dropped++;
    s->dropped_since++;
    return NULL;
}

void cap_stream_drop_frame(struct cap_stream *s)
{
    s->writing = -1;
}

void cap_stream_commit_frame(struct cap_stream *s, uint32_t slot, uint64_t pts_ns, uint32_t flags)
{
    s->writing = -1;
    if (slot >= s->slot_count) return;
    uint64_t now = plh_now_ns();
    struct plh_cap_frame f = {
        .stream_id = s->id, .slot = slot, .frame_id = PLH_FRAME_ID(s->id, ++s->frame_no),
        .pts_ns = pts_ns, .ready_ns = now, .width = s->width, .height = s->height,
        .stride = s->stride, .format = s->format, .flags = flags,
        .dropped = (uint32_t)s->dropped_since,
    };
    s->last_copy_ns = now > pts_ns ? now - pts_ns : 0;
    s->slot_held[slot] = 1;
    s->next_slot = (slot + 1) % s->slot_count;
    s->dropped_since = 0;
    s->sent++;
    s->last_sent_ns = now;
    plh_send_event(s->client, PLH_MSG_CAP_FRAME, &f, sizeof f);
}

void cap_stream_cursor(struct cap_stream *s, int visible, int32_t x, int32_t y, const uint8_t *bgra,
                       uint32_t w, uint32_t h, uint32_t src_stride, int32_t hot_x, int32_t hot_y)
{
    struct plh_cap_cursor c = s->last_cursor;
    c.stream_id = s->id;
    c.visible = visible ? 1 : 0;
    c.x = x;
    c.y = y;
    c.pts_ns = plh_now_ns();
    c.shape_changed = 0;
    if (bgra && w && h) {
        if (w > s->cursor_max) w = s->cursor_max;
        if (h > s->cursor_max) h = s->cursor_max;
        s->cursor_serial++;
        uint8_t *dst = (uint8_t *)s->shm.map + s->cursor_offset + (uint64_t)(s->cursor_serial & 1) * s->cursor_size;
        for (uint32_t row = 0; row < h; row++) memcpy(dst + (size_t)row * w * 4, bgra + (size_t)row * src_stride, (size_t)w * 4);
        c.shape_serial = s->cursor_serial;
        c.shape_changed = 1;
        c.width = w; c.height = h; c.hot_x = hot_x; c.hot_y = hot_y;
    }
    if (!c.shape_changed && c.visible == s->last_cursor.visible && c.x == s->last_cursor.x && c.y == s->last_cursor.y &&
        s->last_cursor.stream_id)
        return;
    s->last_cursor = c;
    plh_send_event(s->client, PLH_MSG_CAP_CURSOR, &c, sizeof c);
}

void cap_stream_state(struct cap_stream *s, uint32_t state, int32_t status, const char *msg)
{
    struct plh_cap_state st = { .stream_id = s->id, .state = state, .status = status };
    if (msg) strncpy(st.message, msg, sizeof st.message - 1);
    PLH_INFO(MOD, "stream %u (%s/%s): state %u %d %s", s->id, s->out.name, s->be->name, state, status, msg ? msg : "");
    plh_send_event(s->client, PLH_MSG_CAP_STATE, &st, sizeof st);
}

/* ---------------------------------------------------------------- stream */
static void stream_free(struct cap_stream *s)
{
    if (s->be && s->be->stop) s->be->stop(s);
    plh_shm_destroy(&s->shm);
    free(s);
}

static void stream_close(struct cap_stream *s)
{
    struct cap_stream **pp;
    for (pp = &streams; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    PLH_INFO(MOD, "stream %u closed (captured %llu sent %llu dropped %llu)", s->id,
             (unsigned long long)s->captured, (unsigned long long)s->sent, (unsigned long long)s->dropped);
    stream_free(s);
}

static void handle_open(struct plh_client *c, const struct plh_msg_hdr *h, const void *p)
{
    struct plh_cap_open req = { 0 };
    memcpy(&req, p, h->size < sizeof req ? h->size : sizeof req);
    struct plh_cap_output outs[CAP_MAX_OUTPUTS];
    int n = cap_outputs_query(outs, CAP_MAX_OUTPUTS);
    const struct plh_cap_output *out = NULL;
    for (int i = 0; i < n; i++) if (outs[i].output_id == req.output_id) out = &outs[i];
    uint32_t bid = req.backend ? req.backend : default_backend();
    if (!out && bid == PLH_CAP_BACKEND_TEST && n <= 0 && req.output_id >= 1 && req.output_id <= 3) {
        /* without Wayland: 3 fake outputs for the test pattern */
        static struct plh_cap_output fake;
        memset(&fake, 0, sizeof fake);
        fake.output_id = req.output_id; fake.x = (int32_t)(req.output_id - 1) * 1920; fake.width = 1920;
        fake.height = 1080; fake.refresh_mhz = 60000; fake.scale_x1000 = 1000;
        snprintf(fake.name, sizeof fake.name, "TEST-%u", req.output_id);
        out = &fake;
    }
    if (!out) { plh_reply_error(c, h, PLH_E_NOENT, "unknown output %u", req.output_id); return; }

    struct cap_stream *s = calloc(1, sizeof *s);
    if (!s) { plh_reply_error(c, h, PLH_E_NOMEM, "memory"); return; }
    s->id = next_stream_id++;
    s->client = c;
    s->out = *out;
    s->flags = req.flags ? req.flags : PLH_CAP_F_CURSOR_METADATA;
    s->max_fps = req.max_fps;
    s->format = req.format ? req.format : PLH_FMT_BGRA8;
    if (s->format != PLH_FMT_BGRA8 && s->format != PLH_FMT_BGRX8) s->format = PLH_FMT_BGRA8;
    s->writing = -1;
    s->width = out->width;
    s->height = out->height;
    s->stride = s->width * 4;
    s->slot_count = req.slot_count ? req.slot_count : 3;
    if (s->slot_count < 2) s->slot_count = 2;
    if (s->slot_count > PLH_CAP_MAX_SLOTS) s->slot_count = PLH_CAP_MAX_SLOTS;
    s->slot_size = (uint32_t)(((uint64_t)s->stride * s->height + 4095) & ~4095ull);
    s->slot_offset = 4096;
    s->cursor_max = 256;
    s->cursor_size = s->cursor_max * s->cursor_max * 4;
    s->cursor_offset = s->slot_offset + (uint64_t)s->slot_count * s->slot_size;
    uint64_t total = s->cursor_offset + 2ull * s->cursor_size;
    char tag[32];
    snprintf(tag, sizeof tag, "cap%u", s->id);
    if (plh_shm_create(&s->shm, tag, total) < 0) {
        free(s);
        plh_reply_error(c, h, PLH_E_NOMEM, "shm %llu bytes", (unsigned long long)total);
        return;
    }
    s->hdr = s->shm.map;
    *s->hdr = (struct plh_cap_shm_header){ PLH_CAP_SHM_MAGIC, 1, s->width, s->height, s->stride, s->format,
                                           s->slot_count, s->slot_size, s->slot_offset, s->cursor_offset,
                                           s->cursor_size, s->cursor_max };

    /* AUTO: detection from the session (cap_detect.c) */
    const struct cap_backend_ops *order[6] = { 0 };
    if (bid == PLH_CAP_BACKEND_AUTO) cap_detect_order(order, 5);
    else order[0] = backend_by_id(bid);
    if (!order[0]) { plh_shm_destroy(&s->shm); free(s); plh_reply_error(c, h, PLH_E_INVAL, "backend %u", bid); return; }

    struct plh_cap_open_reply r = { 0 };
    int rc = PLH_E_UNKNOWN;
    s->next = streams;
    streams = s;
    for (int i = 0; order[i]; i++) {
        s->be = order[i];
        rc = s->be->start(s, out);
        if (rc == PLH_OK || rc == PLH_E_AGAIN) break;
        PLH_WARN(MOD, "backend %s unavailable for %s (%d)", s->be->name, out->name, rc);
        s->be_data = NULL;
    }
    if (rc != PLH_OK && rc != PLH_E_AGAIN) {
        s->be = NULL;
        stream_close(s);
        plh_reply_error(c, h, rc, "no capture backend started for %s", out->name);
        return;
    }
    r.status = rc;
    r.stream_id = s->id;
    r.width = s->width; r.height = s->height; r.stride = s->stride; r.format = s->format;
    r.slot_count = s->slot_count; r.slot_size = s->slot_size; r.slot_offset = s->slot_offset;
    r.cursor_offset = s->cursor_offset; r.cursor_size = s->cursor_size; r.cursor_max = s->cursor_max;
    r.shm_size = s->shm.size;
    r.backend = s->be->id;
    snprintf(r.shm_name, sizeof r.shm_name, "%s", s->shm.name);
    PLH_INFO(MOD, "stream %u: client %u, output %s (%d,%d %ux%u), backend %s, shm %s", s->id, plh_client_id(c),
             out->name, out->x, out->y, out->width, out->height, s->be->name, s->shm.name);
    plh_reply(c, h, &r, sizeof r);
}

static void handle(struct plh_client *c, const struct plh_msg_hdr *h, const void *p)
{
    switch (h->type) {
    case PLH_MSG_CAP_LIST_OUTPUTS: {
        struct plh_cap_output outs[CAP_MAX_OUTPUTS];
        int n = cap_outputs_query(outs, CAP_MAX_OUTPUTS);
        if (n < 0) { plh_reply_error(c, h, PLH_E_IO, "cannot enumerate outputs"); return; }
        struct plh_cap_output_list l = { (uint32_t)n, 0 };
        plh_send2(c, h->type, PLH_F_REPLY, h->seq, &l, sizeof l, outs, (uint32_t)(n * (int)sizeof outs[0]));
        break;
    }
    case PLH_MSG_CAP_OPEN:
        handle_open(c, h, p);
        break;
    case PLH_MSG_CAP_CLOSE: {
        struct plh_cap_stream_ref ref = { 0 };
        memcpy(&ref, p, h->size < sizeof ref ? h->size : sizeof ref);
        struct cap_stream *s = find_stream(c, ref.stream_id);
        if (!s) { plh_reply_error(c, h, PLH_E_NOENT, "stream %u", ref.stream_id); return; }
        stream_close(s);
        plh_reply(c, h, NULL, 0);
        break;
    }
    case PLH_MSG_CAP_RELEASE: {
        struct plh_cap_release rel = { 0 };
        memcpy(&rel, p, h->size < sizeof rel ? h->size : sizeof rel);
        struct cap_stream *s = find_stream(c, rel.stream_id);
        if (s && rel.slot < s->slot_count) s->slot_held[rel.slot] = 0;
        break;
    }
    case PLH_MSG_CAP_STATS: {
        struct plh_cap_stream_ref ref = { 0 };
        memcpy(&ref, p, h->size < sizeof ref ? h->size : sizeof ref);
        struct cap_stream *s = find_stream(c, ref.stream_id);
        if (!s) { plh_reply_error(c, h, PLH_E_NOENT, "stream %u", ref.stream_id); return; }
        struct plh_cap_stats st = { s->id, 0, s->captured, s->sent, s->dropped, s->last_copy_ns };
        plh_reply(c, h, &st, sizeof st);
        break;
    }
    case PLH_MSG_VOUT_CREATE:
    case PLH_MSG_VOUT_DESTROY:
    case PLH_MSG_DISPLAY_STATE:
        cap_vout_handle(c, h, p);
        break;
    default:
        if (h->seq) plh_reply_error(c, h, PLH_E_UNKNOWN, "type 0x%04x", h->type);
    }
}

static void client_gone(struct plh_client *c)
{
    cap_vout_client_gone(c);
    struct cap_stream *s = streams;
    while (s) {
        struct cap_stream *n = s->next;
        if (s->client == c) stream_close(s);
        s = n;
    }
}

static void fini(void)
{
    while (streams) stream_close(streams);
    cap_vout_fini();
}

static int init(void)
{
    return cap_vout_init();
}

static const struct plh_service cap_service = {
    .name = "capture", .id = PLH_SVC_CAPTURE, .init = init, .handle = handle, .client_gone = client_gone, .fini = fini,
};
PLH_SERVICE_REGISTER(cap_service)
