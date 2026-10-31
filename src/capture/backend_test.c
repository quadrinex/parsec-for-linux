/* backend_test.c — animated synthetic test pattern (tests without a desktop or approval). Owner: capture.
 * Image: background whose color depends on the output id, moving vertical bar (1 px/frame),
 * frame number encoded in the clear in the first 64 pixels of row 0 (1 bit per pixel:
 * white = 1, black = 0, least significant first) — checkable by tests.
 * Synthetic 32x32 cursor moving in a circle. */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cap_internal.h"

struct test_be {
    int      timer;
    uint64_t n;
    uint8_t  cursor[32 * 32 * 4];
};

static void tick(void *d)
{
    struct cap_stream *s = d;
    struct test_be *t = s->be_data;
    uint64_t pts = plh_now_ns();
    uint32_t slot;
    t->n++;
    uint8_t *px = cap_stream_begin_frame(s, &slot);
    if (px) {
        uint32_t id = s->out.output_id;
        uint8_t bg[4] = { (uint8_t)(id & 1 ? 200 : 40), (uint8_t)(id & 2 ? 200 : 40), (uint8_t)(id & 4 ? 200 : 40), 255 };
        uint32_t bgv, bar = 0xffffffffu;
        memcpy(&bgv, bg, 4);
        uint32_t bx = (uint32_t)(t->n % s->width);
        for (uint32_t y = 0; y < s->height; y++) {
            uint32_t *row = (uint32_t *)(px + (size_t)y * s->stride);
            for (uint32_t x = 0; x < s->width; x++) row[x] = bgv;
            for (uint32_t x = bx; x < bx + 16 && x < s->width; x++) row[x] = bar;
        }
        uint32_t *row0 = (uint32_t *)px;
        for (int b = 0; b < 64; b++) row0[b] = ((s->frame_no + 1) >> b) & 1 ? 0xffffffffu : 0xff000000u;
        cap_stream_commit_frame(s, slot, pts, 0);
    }
    double a = (double)t->n / 60.0;
    int32_t cx = (int32_t)(s->width / 2 + cos(a) * 300), cy = (int32_t)(s->height / 2 + sin(a) * 300);
    cap_stream_cursor(s, 1, cx, cy, t->n == 1 ? t->cursor : NULL, 32, 32, 32 * 4, 0, 0);
}

static int test_start(struct cap_stream *s, const struct plh_cap_output *out)
{
    (void)out;
    struct test_be *t = calloc(1, sizeof *t);
    if (!t) return PLH_E_NOMEM;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            uint8_t *p = t->cursor + (y * 32 + x) * 4;
            int in = x <= y && x < 20; /* triangular arrow */
            int edge = in && (x == 0 || x == y || x == 19 || y == 31);
            p[0] = p[1] = p[2] = edge ? 0 : 255;
            p[3] = in ? 255 : 0;
        }
    uint32_t fps = s->max_fps ? s->max_fps : (s->out.refresh_mhz ? (s->out.refresh_mhz + 500) / 1000 : 60);
    if (fps > 240) fps = 240;
    s->be_data = t;
    t->timer = plh_timer_add(1000000, 1000000000ull / fps, tick, s);
    if (t->timer < 0) { free(t); s->be_data = NULL; return PLH_E_IO; }
    cap_stream_state(s, PLH_CAP_STATE_STREAMING, 0, "test pattern");
    return PLH_OK;
}

static void test_stop(struct cap_stream *s)
{
    struct test_be *t = s->be_data;
    if (!t) return;
    plh_timer_del(t->timer);
    free(t);
    s->be_data = NULL;
}

const struct cap_backend_ops cap_backend_test = { "test", PLH_CAP_BACKEND_TEST, test_start, test_stop };
