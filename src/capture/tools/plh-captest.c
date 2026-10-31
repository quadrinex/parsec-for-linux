/*
 * plh-captest — test client of the capture service, built natively AND as PE (mingw, under Wine).
 * Owner: capture.
 *
 *   plh-captest [-b auto|portal|kwin|x11|test] [-n frames] [-t seconds] [-o dir] [-s id,id,...] [-q]
 *
 * -t: duration mode (real desktop: KWin only sends a frame when the screen changes); success if each
 *      stream received at least one frame. -n: wait for n frames per stream (test pattern).
 *
 * Lists the outputs, opens one stream per output, receives n frames per stream (default 120), measures
 * fps and capture->receive latency, checks the test pattern (test backend: frame counter on
 * row 0) and writes <dir>/<output>.png (last frame, cursor embedded) + <output>-cursor.png.
 * Exit code 0 if each stream delivered its frames and (test pattern) if the counters are consistent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../bridge/client/plh_client.h"
#ifdef _WIN32
#include <windows.h>
#endif

/* ---------------------------------------------------------------- minimal PNG (deflate "stored") */
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc_upd(0xffffffffu, (const uint8_t *)type, 4);
    c = crc_upd(c, d, n) ^ 0xffffffffu;
    be32(h, c);
    fwrite(h, 1, 4, f);
}
/* bgra -> PNG RGBA 8 bits */
static int write_png(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    size_t nblk = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblk * 5 + 4;
    uint8_t *z = malloc(zlen), *raw = malloc(raw_len);
    if (!z || !raw) { fclose(f); free(z); free(raw); return -1; }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (1 + (size_t)w * 4);
        const uint8_t *s = bgra + (size_t)y * stride;
        r[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            r[1 + x * 4] = s[x * 4 + 2]; r[2 + x * 4] = s[x * 4 + 1]; r[3 + x * 4] = s[x * 4]; r[4 + x * 4] = s[x * 4 + 3];
        }
    }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[o++] = i + n == raw_len;
        z[o++] = n & 0xff; z[o++] = n >> 8; z[o++] = ~n & 0xff; z[o++] = (~n >> 8) & 0xff;
        memcpy(z + o, raw + i, n);
        o += n;
        for (size_t k = 0; k < n; k++) { a = (a + raw[i + k]) % 65521; b = (b + a) % 65521; }
    }
    be32(z + o, (b << 16) | a);
    o += 4;
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    free(z); free(raw);
    return fclose(f);
}

/* ---------------------------------------------------------------- state */
struct st {
    struct plh_cap_output out;
    struct plh_cap_open_reply rep;
    struct plh_shm_map shm;
    uint32_t frames, bad_counter, dropped;
    uint64_t first_ns, last_ns, lat_sum_ns, lat_max_ns;
    int64_t  last_counter;
    int      have_last;
    uint8_t *last;            /* copy of the last frame */
    struct plh_cap_cursor cur;
    int      cursor_events;
    uint32_t state;
};

static struct st streams[16];
static int nstreams;
static struct plh_conn *conn;
static int is_test_backend;
static int64_t clock_offset_ns; /* client - daemon */

static struct st *by_stream(uint32_t id)
{
    for (int i = 0; i < nstreams; i++) if (streams[i].rep.stream_id == id) return &streams[i];
    return NULL;
}

static void on_event(void *u, const struct plh_msg_hdr *h, const void *p)
{
    (void)u;
    if (h->type == PLH_MSG_CAP_FRAME) {
        const struct plh_cap_frame *f = p;
        struct st *s = by_stream(f->stream_id);
        if (!s || !s->shm.ptr) {
            struct plh_cap_release rel = { f->stream_id, f->slot };
            plh_conn_send(conn, PLH_MSG_CAP_RELEASE, &rel, sizeof rel);
            return;
        }
        uint64_t now = plh_client_now_ns();
        int64_t lat = (int64_t)now - clock_offset_ns - (int64_t)f->pts_ns;
        if (lat < 0) lat = 0;
        if (!s->frames) s->first_ns = now;
        s->last_ns = now;
        s->frames++;
        s->lat_sum_ns += (uint64_t)lat;
        if ((uint64_t)lat > s->lat_max_ns) s->lat_max_ns = (uint64_t)lat;
        s->dropped += f->dropped;
        const uint8_t *px = (const uint8_t *)s->shm.ptr + s->rep.slot_offset + (uint64_t)f->slot * s->rep.slot_size;
        if (is_test_backend) {
            const uint32_t *row0 = (const uint32_t *)px;
            int64_t v = 0;
            for (int b = 0; b < 40; b++) if ((row0[b] & 0xffffff) == 0xffffff) v |= (int64_t)1 << b;
            int64_t expect = (int64_t)(f->frame_id & 0xffffffffffull);
            if (v != expect) s->bad_counter++;
            s->last_counter = v;
        }
        if (!s->last) s->last = malloc((size_t)f->stride * f->height);
        if (s->last) memcpy(s->last, px, (size_t)f->stride * f->height);
        s->have_last = 1;
        struct plh_cap_release rel = { f->stream_id, f->slot };
        plh_conn_send(conn, PLH_MSG_CAP_RELEASE, &rel, sizeof rel);
    } else if (h->type == PLH_MSG_CAP_CURSOR) {
        const struct plh_cap_cursor *c = p;
        struct st *s = by_stream(c->stream_id);
        if (!s) return;
        if (c->shape_changed || !s->cursor_events) s->cur = *c;
        else { s->cur.x = c->x; s->cur.y = c->y; s->cur.visible = c->visible; }
        s->cursor_events++;
    } else if (h->type == PLH_MSG_CAP_STATE) {
        const struct plh_cap_state *c = p;
        struct st *s = by_stream(c->stream_id);
        fprintf(stderr, "stream state %u: %u (%d) %s\n", c->stream_id, c->state, c->status, c->message);
        if (s) s->state = c->state;
    }
}

static void composite_cursor(struct st *s, uint8_t *img)
{
    struct plh_cap_cursor *c = &s->cur;
    if (!c->visible || !c->shape_serial) return;
    const uint8_t *src = (const uint8_t *)s->shm.ptr + s->rep.cursor_offset + (uint64_t)(c->shape_serial & 1) * s->rep.cursor_size;
    for (uint32_t y = 0; y < c->height; y++)
        for (uint32_t x = 0; x < c->width; x++) {
            int dx = c->x - c->hot_x + (int)x, dy = c->y - c->hot_y + (int)y;
            if (dx < 0 || dy < 0 || dx >= (int)s->rep.width || dy >= (int)s->rep.height) continue;
            const uint8_t *sp = src + ((size_t)y * c->width + x) * 4;
            uint8_t *dp = img + (size_t)dy * s->rep.stride + (size_t)dx * 4;
            for (int k = 0; k < 3; k++) dp[k] = (uint8_t)((sp[k] * sp[3] + dp[k] * (255 - sp[3])) / 255);
        }
}

int main(int argc, char **argv)
{
    uint32_t backend = PLH_CAP_BACKEND_AUTO, nframes = 120, quiet = 0;
    double duration = 0;
    const char *outdir = ".", *sel = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            const char *b = argv[++i];
            backend = !strcmp(b, "test") ? PLH_CAP_BACKEND_TEST : !strcmp(b, "portal") ? PLH_CAP_BACKEND_PORTAL
                    : !strcmp(b, "kwin") ? PLH_CAP_BACKEND_KWIN : !strcmp(b, "x11") ? PLH_CAP_BACKEND_X11 : PLH_CAP_BACKEND_AUTO;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) nframes = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sel = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else { fprintf(stderr, "usage: plh-captest [-b auto|portal|kwin|x11|test] [-n frames] [-t s] [-o dir] [-s id,..] [-q]\n"); return 2; }
    }
    is_test_backend = backend == PLH_CAP_BACKEND_TEST;
    crc_init();

    struct plh_hello_reply hr;
    int r = plh_conn_open(&conn, PLH_CLIENT_TEST, "plh-captest", &hr);
    if (r) { fprintf(stderr, "cannot connect to the bridge: %d\n", r); return 1; }
    printf("connected (%s): protocol %u.%u, services 0x%x, client %u\n", plh_conn_is_tcp(conn) ? "tcp" : "unix",
           hr.version_major, hr.version_minor, hr.services, hr.client_id);
    plh_conn_set_event_cb(conn, on_event, NULL);

    /* client/daemon clock offset by PING (middle of the round trip) */
    {
        int64_t best_rtt = INT64_MAX;
        for (int i = 0; i < 20; i++) {
            uint8_t buf[64];
            uint32_t l;
            uint64_t t0 = plh_client_now_ns();
            if (plh_conn_request(conn, PLH_MSG_PING, NULL, 0, buf, sizeof buf, &l, 2000)) break;
            uint64_t t1 = plh_client_now_ns();
            struct plh_pong pg;
            memcpy(&pg, buf, sizeof pg);
            int64_t rtt = (int64_t)(t1 - t0);
            if (rtt < best_rtt) { best_rtt = rtt; clock_offset_ns = (int64_t)(t0 + t1) / 2 - (int64_t)pg.daemon_time_ns; }
        }
        printf("ping: RTT min %.1f us\n", best_rtt / 1000.0);
    }

    uint8_t lbuf[sizeof(struct plh_cap_output_list) + 16 * sizeof(struct plh_cap_output)];
    uint32_t ll = 0;
    r = plh_conn_request(conn, PLH_MSG_CAP_LIST_OUTPUTS, NULL, 0, lbuf, sizeof lbuf, &ll, 5000);
    struct plh_cap_output_list *lst = (void *)lbuf;
    int nout = r ? 0 : (int)lst->count;
    struct plh_cap_output *outs = (void *)(lbuf + sizeof *lst);
    if (r) fprintf(stderr, "LIST_OUTPUTS: error %d\n", r);
    for (int i = 0; i < nout; i++)
        printf("output %u: %-10s %5d,%-5d %ux%u @%.3f Hz scale %.2f rot %u %s\n", outs[i].output_id, outs[i].name,
               outs[i].x, outs[i].y, outs[i].width, outs[i].height, outs[i].refresh_mhz / 1000.0,
               outs[i].scale_x1000 / 1000.0, outs[i].rotation, outs[i].description);
    if (nout == 0 && is_test_backend) {
        nout = 3;
        for (int i = 0; i < 3; i++) { memset(&outs[i], 0, sizeof outs[i]); outs[i].output_id = (uint32_t)i + 1; snprintf(outs[i].name, PLH_NAME_MAX, "TEST-%d", i + 1); }
    }

    for (int i = 0; i < nout && nstreams < 16; i++) {
        if (sel) {
            char idb[16];
            snprintf(idb, sizeof idb, "%u", outs[i].output_id);
            const char *p = strstr(sel, idb);
            if (!p) continue;
        }
        struct st *s = &streams[nstreams];
        memset(s, 0, sizeof *s);
        s->out = outs[i];
        struct plh_cap_open op = { outs[i].output_id, backend, PLH_CAP_F_CURSOR_METADATA, 0, 3, PLH_FMT_BGRA8 };
        uint32_t rl;
        r = plh_conn_request(conn, PLH_MSG_CAP_OPEN, &op, sizeof op, &s->rep, sizeof s->rep, &rl, 120000);
        if (r && r != PLH_E_AGAIN) { fprintf(stderr, "CAP_OPEN %s: error %d (%s)\n", outs[i].name, r, ((struct plh_error *)&s->rep)->message); continue; }
        if (plh_shm_map(&s->shm, s->rep.shm_name, s->rep.shm_size)) { fprintf(stderr, "shm %s: failed\n", s->rep.shm_name); continue; }
        printf("stream %u: %s backend %u, %ux%u stride %u, %u slots, shm %s\n", s->rep.stream_id, outs[i].name,
               s->rep.backend, s->rep.width, s->rep.height, s->rep.stride, s->rep.slot_count, s->rep.shm_name);
        nstreams++;
    }
    if (!nstreams) { fprintf(stderr, "no stream opened\n"); plh_conn_close(conn); return 1; }

    uint64_t t0 = plh_client_now_ns();
    uint64_t timeout_ns = duration > 0 ? (uint64_t)(duration * 1e9) : (uint64_t)(nframes / 20 + 30) * 1000000000ull;
    for (;;) {
        int done = 1;
        for (int i = 0; i < nstreams; i++) if (streams[i].frames < nframes) done = 0;
        if (done && duration <= 0) break;
        if (plh_client_now_ns() - t0 > timeout_ns) { if (duration <= 0) fprintf(stderr, "timeout exceeded\n"); break; }
        if (plh_conn_poll(conn, 100) < 0) { fprintf(stderr, "connection lost\n"); break; }
    }

    int rc = 0;
    for (int i = 0; i < nstreams; i++) {
        struct st *s = &streams[i];
        double dur = (double)(s->last_ns - s->first_ns) / 1e9;
        double fps = s->frames > 1 && dur > 0 ? (s->frames - 1) / dur : 0;
        if (duration > 0) fps = s->frames / duration;
        printf("RESULT output=%s pos=%d,%d size=%ux%u frames=%u fps=%.2f latency_avg_ms=%.3f latency_max_ms=%.3f "
               "lost=%u cursor_events=%d cursor=%d,%d %ux%u",
               s->out.name, s->out.x, s->out.y, s->rep.width, s->rep.height, s->frames, fps,
               s->frames ? s->lat_sum_ns / 1e6 / s->frames : 0.0, s->lat_max_ns / 1e6, s->dropped,
               s->cursor_events, s->cur.x, s->cur.y, s->cur.width, s->cur.height);
        if (is_test_backend) printf(" counter_errors=%u", s->bad_counter);
        printf("\n");
        if ((duration > 0 ? s->frames < 1 : s->frames < nframes) || (is_test_backend && s->bad_counter)) rc = 1;
        if (s->have_last && !quiet) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s.png", outdir, s->out.name);
            composite_cursor(s, s->last);
            if (write_png(path, s->last, s->rep.width, s->rep.height, s->rep.stride)) { fprintf(stderr, "writing %s: failed\n", path); rc = 1; }
            else printf("written %s\n", path);
            if (s->cur.shape_serial) {
                snprintf(path, sizeof path, "%s/%s-cursor.png", outdir, s->out.name);
                const uint8_t *src = (const uint8_t *)s->shm.ptr + s->rep.cursor_offset + (uint64_t)(s->cur.shape_serial & 1) * s->rep.cursor_size;
                write_png(path, src, s->cur.width, s->cur.height, s->cur.width * 4);
            }
        }
        struct plh_cap_stream_ref ref = { s->rep.stream_id, 0 };
        plh_conn_request(conn, PLH_MSG_CAP_CLOSE, &ref, sizeof ref, NULL, 0, NULL, 2000);
        plh_shm_unmap(&s->shm);
        free(s->last);
    }
    plh_conn_close(conn);
    return rc;
}
