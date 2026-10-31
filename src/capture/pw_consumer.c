/* pw_consumer.c — PipeWire consumer integrated into the daemon epoll loop. Owner: capture.
 * Phase 1: memory buffers (MemFd/MemPtr) copied into the /dev/shm slots.
 * Not implemented: offer DmaBuf (modifiers) and keep the DMA-BUF tied to the frame_id for the encoder. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/utils/result.h>

#include "pw_consumer.h"

#define MOD "pipewire"
#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)

/* PipeWire context shared per connection (remote fd or local daemon) */
struct pw_conn {
    int                fd_key;     /* portal fd, or -1 */
    struct pw_context *ctx;
    struct pw_core    *core;
    struct spa_hook    core_listener;
    int                refs;
    struct pw_conn    *next;
};

static struct pw_loop *loop;
static int loop_fd = -1;
static struct pw_conn *conns;

struct cap_pw {
    struct cap_stream *s;
    struct pw_conn    *conn;
    struct pw_stream  *stream;
    struct spa_hook    listener;
    struct spa_video_info_raw fmt;
    int                have_fmt;
    uint64_t           frames;
    int                warned_size;
};

static void loop_cb(int fd, uint32_t ev, void *d)
{
    (void)fd; (void)ev; (void)d;
    pw_loop_enter(loop);
    pw_loop_iterate(loop, 0);
    pw_loop_leave(loop);
}

static int ensure_loop(void)
{
    if (loop) return 0;
    pw_init(NULL, NULL);
    loop = pw_loop_new(NULL);
    if (!loop) return -1;
    loop_fd = pw_loop_get_fd(loop);
    return plh_loop_add_fd(loop_fd, EPOLLIN, loop_cb, NULL);
}

static void core_error(void *d, uint32_t id, int seq, int res, const char *msg)
{
    (void)d;
    PLH_WARN(MOD, "core error id %u seq %d: %s (%s)", id, seq, msg, spa_strerror(res));
}
static const struct pw_core_events core_events = { PW_VERSION_CORE_EVENTS, .error = core_error };

static struct pw_conn *conn_get(int fd)
{
    for (struct pw_conn *c = conns; c; c = c->next)
        if (c->fd_key == fd) { c->refs++; return c; }
    struct pw_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd_key = fd;
    c->ctx = pw_context_new(loop, NULL, 0);
    if (!c->ctx) { free(c); return NULL; }
    /* pw_context_connect_fd takes ownership of the fd: give it a copy */
    c->core = fd >= 0 ? pw_context_connect_fd(c->ctx, dup(fd), NULL, 0) : pw_context_connect(c->ctx, NULL, 0);
    if (!c->core) {
        PLH_ERR(MOD, "cannot connect to PipeWire");
        pw_context_destroy(c->ctx);
        free(c);
        return NULL;
    }
    pw_core_add_listener(c->core, &c->core_listener, &core_events, c);
    c->refs = 1;
    c->next = conns;
    conns = c;
    return c;
}

static void conn_put(struct pw_conn *c)
{
    if (--c->refs > 0) return;
    struct pw_conn **pp;
    for (pp = &conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    spa_hook_remove(&c->core_listener);
    pw_core_disconnect(c->core);
    pw_context_destroy(c->ctx);
    free(c);
}

/* ---------------------------------------------------------------- stream */
static void on_state(void *d, enum pw_stream_state old, enum pw_stream_state st, const char *err)
{
    struct cap_pw *p = d;
    (void)old;
    PLH_INFO(MOD, "stream %u: %s%s%s", p->s->id, pw_stream_state_as_string(st), err ? ": " : "", err ? err : "");
    if (st == PW_STREAM_STATE_STREAMING) cap_stream_state(p->s, PLH_CAP_STATE_STREAMING, 0, "pipewire");
    else if (st == PW_STREAM_STATE_PAUSED && old == PW_STREAM_STATE_STREAMING) cap_stream_state(p->s, PLH_CAP_STATE_PAUSED, 0, "pipewire");
    else if (st == PW_STREAM_STATE_ERROR) cap_stream_state(p->s, PLH_CAP_STATE_ERROR, PLH_E_IO, err);
    else if (st == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_CONNECTING) cap_stream_state(p->s, PLH_CAP_STATE_CLOSED, 0, "pipewire");
}

static void on_param_changed(void *d, uint32_t id, const struct spa_pod *param)
{
    struct cap_pw *p = d;
    if (id != SPA_PARAM_Format || !param) return;
    uint32_t mt, mst;
    if (spa_format_parse(param, &mt, &mst) < 0 || mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw) return;
    memset(&p->fmt, 0, sizeof p->fmt);
    spa_format_video_raw_parse(param, &p->fmt);
    p->have_fmt = 1;
    PLH_INFO(MOD, "stream %u: format %s %ux%u, %u/%u fps, modifier 0x%llx", p->s->id,
             spa_debug_type_find_short_name(spa_type_video_format, p->fmt.format), p->fmt.size.width,
             p->fmt.size.height, p->fmt.max_framerate.num, p->fmt.max_framerate.denom,
             (unsigned long long)p->fmt.modifier);

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[3];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[2] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1), CURSOR_META_SIZE(256, 256)));
    pw_stream_update_params(p->stream, params, 3);
}

static void copy_cursor(struct cap_pw *p, struct spa_buffer *b)
{
    struct spa_meta_cursor *mc = spa_buffer_find_meta_data(b, SPA_META_Cursor, sizeof *mc);
    if (!mc) return;
    struct cap_stream *s = p->s;
    if (!spa_meta_cursor_is_valid(mc)) {
        /* id 0: KWin has no cursor on this output (pointer elsewhere or hidden): tell the client, keep the last position */
        cap_stream_cursor(s, 0, s->last_cursor.x, s->last_cursor.y, NULL, 0, 0, 0, 0, 0);
        return;
    }
    const uint8_t *bits = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    static uint8_t conv[256 * 256 * 4];
    if (mc->bitmap_offset >= sizeof *mc) {
        struct spa_meta_bitmap *bm = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
        if (bm->format && bm->offset >= sizeof *bm && bm->size.width && bm->size.height) {
            w = bm->size.width; h = bm->size.height; stride = (uint32_t)bm->stride;
            const uint8_t *src = SPA_PTROFF(bm, bm->offset, const uint8_t);
            if (w > 256) w = 256;
            if (h > 256) h = 256;
            /* to BGRA: RGBA/RGBx -> swap R/B */
            int swap = bm->format == SPA_VIDEO_FORMAT_RGBA || bm->format == SPA_VIDEO_FORMAT_RGBx;
            int noalpha = bm->format == SPA_VIDEO_FORMAT_BGRx || bm->format == SPA_VIDEO_FORMAT_RGBx;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t *sp = src + (size_t)y * stride + x * 4;
                    uint8_t *dp = conv + ((size_t)y * w + x) * 4;
                    dp[0] = swap ? sp[2] : sp[0]; dp[1] = sp[1]; dp[2] = swap ? sp[0] : sp[2];
                    dp[3] = noalpha ? 255 : sp[3];
                    /* KWin cursor bitmaps are premultiplied (colour <= alpha, e.g. 226,226,226,229); the bridge contract
                     * and DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR are straight alpha. */
                    if (!noalpha && dp[3] && dp[3] < 255)
                        for (int k = 0; k < 3; k++) { unsigned v = (dp[k] * 255u + dp[3] / 2) / dp[3]; dp[k] = v > 255 ? 255 : (uint8_t)v; }
                }
            bits = conv;
            stride = w * 4;
        }
    }
    /* SPA cursor meta: position is where the pointer (hotspot) is in the stream image; the bitmap's top-left is
     * position - hotspot. The bridge event carries the hotspot position (x, y) and hot_x/hot_y. */
    int32_t x = mc->position.x, y = mc->position.y;
    if (bits) cap_stream_cursor(s, 1, x, y, bits, w, h, stride, mc->hotspot.x, mc->hotspot.y);
    else cap_stream_cursor(s, 1, x, y, NULL, 0, 0, 0, 0, 0);
}

static void on_process(void *d)
{
    struct cap_pw *p = d;
    struct cap_stream *s = p->s;
    struct pw_buffer *pb, *last = NULL;
    /* keep only the most recent one */
    while ((pb = pw_stream_dequeue_buffer(p->stream))) {
        if (last) {
            copy_cursor(p, last->buffer);
            pw_stream_queue_buffer(p->stream, last);
        }
        last = pb;
    }
    if (!last) return;
    struct spa_buffer *b = last->buffer;
    copy_cursor(p, b);
    struct spa_data *dd = &b->datas[0];
    struct spa_meta_header *mh = spa_buffer_find_meta_data(b, SPA_META_Header, sizeof *mh);
    uint64_t pts = plh_now_ns();
    if (mh && mh->pts > 0 && (uint64_t)mh->pts <= pts && pts - (uint64_t)mh->pts < 1000000000ull) pts = (uint64_t)mh->pts;
    int has_video = dd->data && dd->chunk && dd->chunk->size > 0 && !(dd->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) &&
                    !(mh && (mh->flags & SPA_META_HEADER_FLAG_CORRUPTED));
    if (has_video && p->have_fmt) {
        uint32_t w = p->fmt.size.width, h = p->fmt.size.height;
        if (w != s->width || h != s->height) {
            if (!p->warned_size++) PLH_WARN(MOD, "stream %u: PipeWire size %ux%u != output %ux%u (cropping)", s->id, w, h, s->width, s->height);
            if (w > s->width) w = s->width;
            if (h > s->height) h = s->height;
        }
        uint32_t slot;
        uint8_t *dst = cap_stream_begin_frame(s, &slot);
        if (dst) {
            const uint8_t *src = (const uint8_t *)dd->data + dd->chunk->offset;
            uint32_t sstride = dd->chunk->stride ? (uint32_t)dd->chunk->stride : w * 4;
            int fmt = p->fmt.format;
            int swap = fmt == SPA_VIDEO_FORMAT_RGBA || fmt == SPA_VIDEO_FORMAT_RGBx;
            int fill_alpha = fmt == SPA_VIDEO_FORMAT_BGRx || fmt == SPA_VIDEO_FORMAT_RGBx;
            for (uint32_t y = 0; y < h; y++) {
                const uint32_t *sr = (const uint32_t *)(src + (size_t)y * sstride);
                uint32_t *dr = (uint32_t *)(dst + (size_t)y * s->stride);
                if (!swap && !fill_alpha) memcpy(dr, sr, (size_t)w * 4);
                else if (!swap) for (uint32_t x = 0; x < w; x++) dr[x] = sr[x] | 0xff000000u;
                else for (uint32_t x = 0; x < w; x++) {
                    uint32_t v = sr[x];
                    dr[x] = (v & 0xff00ff00u) | ((v >> 16) & 0xff) | ((v & 0xff) << 16) | (fill_alpha ? 0xff000000u : 0);
                }
            }
            cap_stream_commit_frame(s, slot, pts, 0);
            p->frames++;
        }
    }
    pw_stream_queue_buffer(p->stream, last);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS, .state_changed = on_state, .param_changed = on_param_changed, .process = on_process,
};

struct cap_pw *cap_pw_start(struct cap_stream *s, int pw_fd, const char *target, uint32_t node_id)
{
    if (ensure_loop() < 0) return NULL;
    struct cap_pw *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->s = s;
    p->conn = conn_get(pw_fd);
    if (!p->conn) { free(p); return NULL; }
    struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                    PW_KEY_MEDIA_ROLE, "Screen", NULL);
    if (target) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
    char name[96];
    snprintf(name, sizeof name, "plh-capture-%s", s->out.name);
    p->stream = pw_stream_new(p->conn->core, name, props);
    if (!p->stream) { conn_put(p->conn); free(p); return NULL; }
    pw_stream_add_listener(p->stream, &p->listener, &stream_events, p);

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    uint32_t fps = s->max_fps ? s->max_fps : 60;
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                                                        SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(s->width, s->height),
                                                              &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(fps, 1), &SPA_FRACTION(1, 1),
                                                                     &SPA_FRACTION(fps, 1)));
    int r = pw_stream_connect(p->stream, PW_DIRECTION_INPUT, node_id ? node_id : PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    if (r < 0) {
        PLH_ERR(MOD, "pw_stream_connect : %s", spa_strerror(r));
        cap_pw_stop(p);
        return NULL;
    }
    PLH_INFO(MOD, "stream %u: connecting to node %s (id %u)", s->id, target ? target : "?", node_id);
    return p;
}

void cap_pw_stop(struct cap_pw *p)
{
    if (!p) return;
    if (p->stream) {
        spa_hook_remove(&p->listener);
        pw_stream_destroy(p->stream);
    }
    conn_put(p->conn);
    free(p);
}
