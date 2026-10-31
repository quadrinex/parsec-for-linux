/* plh-enctest — client of the bridge encode service (native or PE under Wine).
 * Writes a BGRA (or NV12) test pattern into the shm slots, encodes N frames, writes the stream.
 * usage: plh-enctest [-c h264|hevc] [-s WxH] [-n N] [-i idr1,idr2] [-f bgra|nv12]
 *                    [-b bitrate] [-o output] [-r] (bitrate reconfig mid-run)
 * Last line: RESULT frames=N keys=a,b,... bytes=.. lat_p50_ms=.. lat_max_ms=.. */
#define _POSIX_C_SOURCE 200809L
#include "../../bridge/client/plh_client.h"
#include "../proto/plh_proto_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void draw(uint8_t *p, uint32_t stride, uint32_t w, uint32_t h, uint32_t f, int nv12, uint64_t uvoff)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)(x + f * 4), g = (uint8_t)(y + f * 2), b = (uint8_t)((x ^ y) >> 2);
            if (x / 64 % 8 == f % 8 && y < 64) r = g = b = 255;
            if (nv12) p[(size_t)y * stride + x] = (uint8_t)(16 + (54 * r + 183 * g + 18 * b) * 219 / 255 / 256);
            else {
                uint8_t *q = p + (size_t)y * stride + x * 4;
                q[0] = b; q[1] = g; q[2] = r; q[3] = 255;
            }
        }
    if (nv12)
        for (uint32_t y = 0; y < h / 2; y++)
            memset(p + uvoff + (size_t)y * stride, 128, w);
}

static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *codec = "h264", *out = NULL, *idr = "", *fmt = "bgra";
    uint32_t w = 1920, h = 1080, n = 60, br = 20000000;
    int reconf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "-c")) { codec = v; i++; }
        else if (!strcmp(a, "-s")) { sscanf(v, "%ux%u", &w, &h); i++; }
        else if (!strcmp(a, "-n")) { n = (uint32_t)atoi(v); i++; }
        else if (!strcmp(a, "-i")) { idr = v; i++; }
        else if (!strcmp(a, "-f")) { fmt = v; i++; }
        else if (!strcmp(a, "-b")) { br = (uint32_t)strtoul(v, NULL, 0); i++; }
        else if (!strcmp(a, "-o")) { out = v; i++; }
        else if (!strcmp(a, "-r")) reconf = 1;
        else { fprintf(stderr, "option inconnue %s\n", a); return 2; }
    }
    int nv12 = !strcmp(fmt, "nv12");
    struct plh_conn *c;
    struct plh_hello_reply hr;
    int st = plh_conn_open(&c, PLH_CLIENT_ENCODER, "plh-enctest", &hr);
    if (st) { printf("ERROR connexion : %d\n", st); return 1; }
    printf("connected (%s), services 0x%x\n", plh_conn_is_tcp(c) ? "tcp" : "unix", hr.services);

    struct plh_enc_caps_reply caps;
    uint32_t len;
    st = plh_conn_request(c, PLH_MSG_ENC_CAPS, NULL, 0, &caps, sizeof caps, &len, 5000);
    if (st) { printf("ERROR caps : %d\n", st); return 1; }
    printf("caps : backend=%s codecs=0x%x h264<=%ux%u hevc<=%ux%u (%s)\n", caps.backend, caps.codecs,
           caps.h264_max_w, caps.h264_max_h, caps.hevc_max_w, caps.hevc_max_h, caps.device);

    struct plh_enc_open o;
    memset(&o, 0, sizeof o);
    o.codec = !strcmp(codec, "hevc") ? PLH_ENC_CODEC_HEVC : PLH_ENC_CODEC_H264;
    o.width = w; o.height = h; o.fps_num = 60; o.fps_den = 1;
    o.rc = PLH_ENC_RC_CBR; o.bitrate = br; o.quality = -1;
    o.in_format = nv12 ? PLH_FMT_NV12 : PLH_FMT_BGRA8;
    struct plh_enc_open_reply orp;
    st = plh_conn_request(c, PLH_MSG_ENC_OPEN, &o, sizeof o, &orp, sizeof orp, &len, 5000);
    if (st) {
        struct plh_error *e = (struct plh_error *)&orp;
        printf("ERROR open : %d %s\n", st, len >= sizeof *e ? e->message : "");
        return 1;
    }
    printf("session %u shm=%s slots=%u stride=%u backend=%s\n", orp.session_id, orp.shm_name,
           orp.in_slot_count, orp.in_stride, orp.backend);
    struct plh_shm_map m;
    if (plh_shm_map(&m, orp.shm_name, orp.shm_size)) { printf("ERROR shm_map\n"); return 1; }

    FILE *fo = out ? fopen(out, "wb") : NULL;
    uint64_t *lat = calloc(n, sizeof *lat), bytes = 0;
    char keys[1024] = "";
    for (uint32_t f = 0; f < n; f++) {
        uint32_t slot = f % orp.in_slot_count;
        uint8_t *base = (uint8_t *)m.ptr + orp.in_offset + slot * orp.in_slot_size;
        uint64_t uvoff = (uint64_t)orp.in_stride * h;
        draw(base, orp.in_stride, w, h, f, nv12, uvoff);
        struct plh_enc_frame fr;
        memset(&fr, 0, sizeof fr);
        fr.session_id = orp.session_id;
        fr.source = PLH_ENC_SRC_SLOT;
        fr.slot = slot;
        fr.width = w; fr.height = h; fr.stride = orp.in_stride;
        fr.format = o.in_format;
        fr.uv_offset = nv12 ? uvoff : 0;
        fr.pts = f * 1000ull;
        for (const char *s = idr; *s;) {
            if ((uint32_t)atoi(s) == f && f) fr.flags |= PLH_ENC_FRAME_F_FORCE_IDR;
            s = strchr(s, ',');
            if (!s) break;
            s++;
        }
        if (reconf && f == n / 2) {
            struct plh_enc_reconfig rc = { .session_id = orp.session_id, .bitrate = br / 4 };
            if (plh_conn_request(c, PLH_MSG_ENC_RECONFIG, &rc, sizeof rc, NULL, 0, &len, 5000)) {
                printf("ERROR reconfig\n"); return 1;
            }
        }
        struct plh_enc_frame_reply rr;
        uint64_t t0 = plh_client_now_ns();
        st = plh_conn_request(c, PLH_MSG_ENC_FRAME, &fr, sizeof fr, &rr, sizeof rr, &len, 5000);
        lat[f] = plh_client_now_ns() - t0;
        if (st) {
            struct plh_error *e = (struct plh_error *)&rr;
            printf("ERROR frame %u : %d %s\n", f, st, len >= 8 ? e->message : "");
            return 1;
        }
        if (rr.pts != fr.pts || rr.frame_index != f) { printf("ERROR pts/index\n"); return 1; }
        if (rr.keyframe) {
            size_t l = strlen(keys);
            snprintf(keys + l, sizeof keys - l, "%s%u", l ? "," : "", f);
        }
        bytes += rr.size;
        if (fo) fwrite((uint8_t *)m.ptr + orp.out_offset, 1, rr.size, fo);
    }
    if (fo) fclose(fo);
    struct plh_enc_session_ref ref = { .session_id = orp.session_id };
    st = plh_conn_request(c, PLH_MSG_ENC_CLOSE, &ref, sizeof ref, NULL, 0, &len, 5000);
    qsort(lat, n, sizeof *lat, cmp);
    printf("RESULT frames=%u keys=%s bytes=%llu lat_p50_ms=%.3f lat_max_ms=%.3f close=%d\n", n, keys,
           (unsigned long long)bytes, lat[n / 2] / 1e6, lat[n - 1] / 1e6, st);
    plh_shm_unmap(&m);
    plh_conn_close(c);
    return st ? 1 : 0;
}
