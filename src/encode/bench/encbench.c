/* encbench — native encoder bench (N1).
 *
 * Generates an animated test pattern in DMA-BUFs (as capture would), imports
 * them into the encoder, encodes and measures per-frame latency
 * (submission -> complete packet in CPU memory).
 *
 * Sources:
 *   --src bgrx  : GBM XRGB8888 buffer (rendered, possibly tiled) -> VPP -> NV12 -> encoder
 *   --src nv12  : NV12 surface of another VADisplay exported as DMA-BUF -> encoder
 *   --src cpu   : CPU upload of BGRX (fallback "texture readback")
 *
 * Example: encbench --codec h264 --size 1920x1080 --frames 600 --rc cbr \
 *             --bitrate 20000000 --idr-at 100,250 --out /tmp/plh-encodeur/a.h264
 */
#define _GNU_SOURCE
#include "plh_enc.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#define NBUF 4

static void die(const char *m) { fprintf(stderr, "encbench: %s\n", m); exit(1); }

/* Pattern: scrolling gradient + bands + moving square + noise block (simulated text). */
static void draw_bgrx(uint8_t *px, uint32_t stride, uint32_t w, uint32_t h, uint32_t f)
{
    uint32_t seed = f * 2654435761u;
    uint32_t bx = (f * 13) % (w > 200 ? w - 200 : 1), by = (f * 7) % (h > 200 ? h - 200 : 1);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = (x + f * 4) & 255, g = (y + f * 2) & 255, b = ((x ^ y) >> 2) & 255;
            if (((x / 240) & 1) && y < h / 8) { r = g = b = 255 * ((x / 240) & 2) / 2; }
            if (x >= bx && x < bx + 200 && y >= by && y < by + 200) { r = 255; g = 32; b = 32; }
            if (x >= w / 2 && x < w / 2 + 320 && y >= h / 2 && y < h / 2 + 64) {
                seed = seed * 1103515245u + 12345u;
                uint32_t v = (seed >> 16) & 1 ? 230 : 20;
                r = g = b = v;
            }
            row[x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/* BGRX -> NV12 BT.709 limited range (reference for the nv12 source). */
static void bgrx_to_nv12(const uint8_t *src, uint32_t sst, uint32_t w, uint32_t h,
                         uint8_t *Y, uint32_t ys, uint8_t *UV, uint32_t uvs)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = src + (size_t)y * sst + x * 4;
            double R = p[2], G = p[1], B = p[0];
            Y[(size_t)y * ys + x] = (uint8_t)lrint(16 + (0.2126 * R + 0.7152 * G + 0.0722 * B) * 219 / 255);
        }
    for (uint32_t y = 0; y < h / 2; y++)
        for (uint32_t x = 0; x < w / 2; x++) {
            double R = 0, G = 0, B = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t *p = src + (size_t)(2 * y + dy) * sst + (2 * x + dx) * 4;
                    R += p[2]; G += p[1]; B += p[0];
                }
            R /= 4; G /= 4; B /= 4;
            double cb = (-0.1146 * R - 0.3854 * G + 0.5 * B) * 224 / 255 + 128;
            double cr = (0.5 * R - 0.4542 * G - 0.0458 * B) * 224 / 255 + 128;
            UV[(size_t)y * uvs + 2 * x] = (uint8_t)lrint(cb);
            UV[(size_t)y * uvs + 2 * x + 1] = (uint8_t)lrint(cr);
        }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    plh_enc_params p;
    memset(&p, 0, sizeof p);
    p.codec = PLH_CODEC_H264;
    p.width = 1920; p.height = 1080;
    p.fps_num = 60; p.fps_den = 1;
    p.rc = PLH_RC_CBR;
    p.bitrate = 20000000;
    p.quality = -1;
    int frames = 300;
    const char *src = "bgrx", *out = NULL, *dump = NULL, *idr_at = "", *device = NULL;
    const char *modifier = "linear", *backend = "vaapi";
    int realtime = 0;

    static const struct option opts[] = {
        {"codec", 1, 0, 'c'}, {"size", 1, 0, 's'}, {"frames", 1, 0, 'n'},
        {"rc", 1, 0, 'r'}, {"bitrate", 1, 0, 'b'}, {"peak", 1, 0, 'P'},
        {"qp", 1, 0, 'q'}, {"profile", 1, 0, 'p'}, {"src", 1, 0, 'S'},
        {"out", 1, 0, 'o'}, {"dump-src", 1, 0, 'd'}, {"idr-at", 1, 0, 'i'},
        {"device", 1, 0, 'D'}, {"quality", 1, 0, 'Q'}, {"realtime", 0, 0, 'R'},
        {"modifier", 1, 0, 'm'}, {"backend", 1, 0, 'B'}, {"fps", 1, 0, 'f'}, {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'c': p.codec = !strcmp(optarg, "hevc") ? PLH_CODEC_HEVC : PLH_CODEC_H264; break;
        case 's': if (sscanf(optarg, "%ux%u", &p.width, &p.height) != 2) die("--size WxH"); break;
        case 'n': frames = atoi(optarg); break;
        case 'r': p.rc = !strcmp(optarg, "cqp") ? PLH_RC_CQP : !strcmp(optarg, "vbr") ? PLH_RC_VBR : PLH_RC_CBR; break;
        case 'b': p.bitrate = (uint32_t)strtoul(optarg, 0, 0); break;
        case 'P': p.peak_bitrate = (uint32_t)strtoul(optarg, 0, 0); break;
        case 'q': p.qp_i = p.qp_p = (uint32_t)atoi(optarg); break;
        case 'p': p.profile = !strcmp(optarg, "cb") ? PLH_PROFILE_H264_CB :
                              !strcmp(optarg, "main") ? PLH_PROFILE_H264_MAIN : 0; break;
        case 'S': src = optarg; break;
        case 'o': out = optarg; break;
        case 'd': dump = optarg; break;
        case 'i': idr_at = optarg; break;
        case 'D': device = optarg; break;
        case 'Q': p.quality = atoi(optarg); break;
        case 'R': realtime = 1; break;
        case 'm': modifier = optarg; break;
        case 'B': backend = optarg; break;
        case 'f': p.fps_num = (uint32_t)atoi(optarg); break;
        default: die("unknown option");
        }
    }
    const char *devpath = device ? device : "/dev/dri/renderD128";

    char err[256];
    plh_encoder *enc = plh_enc_create(backend, device, &p, err, sizeof err);
    if (!enc) { fprintf(stderr, "encbench: creation: %s\n", err); return 2; }

    uint32_t W = p.width, H = p.height;
    uint32_t sstride = W * 4;
    uint8_t *pix = malloc((size_t)sstride * H);
    FILE *fo = out ? fopen(out, "wb") : NULL;
    FILE *fd = dump ? fopen(dump, "wb") : NULL;
    if ((out && !fo) || (dump && !fd) || !pix) die("files");

    plh_surface *surf[NBUF] = {0};
    /* sources DMA-BUF */
    int gfd = -1;
    struct gbm_device *gbm = NULL;
    struct gbm_bo *bo[NBUF] = {0};
    int bofd[NBUF];
    VADisplay sdpy = NULL;
    int sfd = -1;
    VASurfaceID ssurf[NBUF];
    uint8_t *ybuf = NULL, *uvbuf = NULL;
    const char *srcdesc = src;
    char descbuf[128];

    if (!strcmp(src, "bgrx")) {
        gfd = open(devpath, O_RDWR | O_CLOEXEC);
        gbm = gfd >= 0 ? gbm_create_device(gfd) : NULL;
        if (!gbm) die("gbm_create_device");
        /* modifier: "linear", "implicit", "auto" (the one the driver picks
         * for an RGB surface, typically AMD tiled), or a 0x... value */
        uint64_t mod = DRM_FORMAT_MOD_INVALID;
        if (!strcmp(modifier, "linear")) mod = DRM_FORMAT_MOD_LINEAR;
        else if (!strcmp(modifier, "auto")) {
            int tfd = open(devpath, O_RDWR | O_CLOEXEC);
            VADisplay t = vaGetDisplayDRM(tfd);
            int a, b2;
            VASurfaceID ts;
            VASurfaceAttrib sa = { .type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
                                   .value = { .type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRX } };
            VADRMPRIMESurfaceDescriptor x;
            if (!t || vaInitialize(t, &a, &b2) ||
                vaCreateSurfaces(t, VA_RT_FORMAT_RGB32, W, H, &ts, 1, &sa, 1) ||
                vaExportSurfaceHandle(t, ts, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &x))
                die("auto: export VA RGB");
            mod = x.objects[0].drm_format_modifier;
            for (uint32_t o = 0; o < x.num_objects; o++) close(x.objects[o].fd);
            vaDestroySurfaces(t, &ts, 1);
            vaTerminate(t);
            close(tfd);
        } else if (strcmp(modifier, "implicit")) mod = strtoull(modifier, NULL, 0);
        for (int i = 0; i < NBUF; i++) {
            if (mod == DRM_FORMAT_MOD_INVALID)
                bo[i] = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
            else
                bo[i] = gbm_bo_create_with_modifiers2(gbm, W, H, GBM_FORMAT_XRGB8888, &mod, 1,
                                                      GBM_BO_USE_RENDERING);
            if (!bo[i]) die("gbm_bo_create");
            plh_dmabuf d;
            memset(&d, 0, sizeof d);
            d.drm_fourcc = DRM_FORMAT_XRGB8888;
            d.width = W; d.height = H;
            d.modifier = gbm_bo_get_modifier(bo[i]);
            d.num_planes = gbm_bo_get_plane_count(bo[i]);
            bofd[i] = gbm_bo_get_fd(bo[i]);
            for (int pl = 0; pl < d.num_planes; pl++) {
                d.fd[pl] = bofd[i];
                d.offset[pl] = gbm_bo_get_offset(bo[i], pl);
                d.pitch[pl] = gbm_bo_get_stride_for_plane(bo[i], pl);
            }
            surf[i] = plh_enc_import_dmabuf(enc, &d);
            if (!surf[i]) { fprintf(stderr, "encbench: %s\n", plh_enc_last_error(enc)); return 3; }
            snprintf(descbuf, sizeof descbuf, "bgrx dmabuf (gbm, modifier 0x%llx, %d plan(s))",
                     (unsigned long long)d.modifier, d.num_planes);
            srcdesc = descbuf;
        }
    } else if (!strcmp(src, "nv12")) {
        sfd = open(devpath, O_RDWR | O_CLOEXEC);
        sdpy = vaGetDisplayDRM(sfd);
        int a, b;
        if (!sdpy || vaInitialize(sdpy, &a, &b) != VA_STATUS_SUCCESS) die("va source");
        if (vaCreateSurfaces(sdpy, VA_RT_FORMAT_YUV420, W, H, ssurf, NBUF, NULL, 0) != VA_STATUS_SUCCESS)
            die("source surfaces");
        ybuf = malloc((size_t)W * H);
        uvbuf = malloc((size_t)W * H / 2);
        for (int i = 0; i < NBUF; i++) {
            VADRMPRIMESurfaceDescriptor x;
            if (vaExportSurfaceHandle(sdpy, ssurf[i], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                      VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                      &x) != VA_STATUS_SUCCESS)
                die("vaExportSurfaceHandle");
            plh_dmabuf d;
            memset(&d, 0, sizeof d);
            d.drm_fourcc = x.layers[0].drm_format;
            d.width = W; d.height = H;
            d.num_planes = (int)x.layers[0].num_planes;
            for (int pl = 0; pl < d.num_planes; pl++) {
                uint32_t o = x.layers[0].object_index[pl];
                d.fd[pl] = x.objects[o].fd;
                d.obj_size[pl] = x.objects[o].size;
                d.modifier = x.objects[o].drm_format_modifier;
                d.offset[pl] = x.layers[0].offset[pl];
                d.pitch[pl] = x.layers[0].pitch[pl];
            }
            surf[i] = plh_enc_import_dmabuf(enc, &d);
            if (!surf[i]) { fprintf(stderr, "encbench: %s\n", plh_enc_last_error(enc)); return 3; }
            snprintf(descbuf, sizeof descbuf, "nv12 dmabuf (VA export, modifier 0x%llx)",
                     (unsigned long long)d.modifier);
            srcdesc = descbuf;
        }
    } else if (!strcmp(src, "cpu")) {
        for (int i = 0; i < NBUF; i++) {
            surf[i] = plh_enc_alloc_surface(enc, DRM_FORMAT_XRGB8888);
            if (!surf[i]) { fprintf(stderr, "encbench: %s\n", plh_enc_last_error(enc)); return 3; }
        }
        srcdesc = "cpu bgrx (upload not measured)";
    } else die("--src bgrx|nv12|cpu");

    /* IDR on demand */
    int nidr = 0, idrs[64];
    for (const char *s = idr_at; *s && nidr < 64;) {
        idrs[nidr++] = atoi(s);
        s = strchr(s, ',');
        if (!s) break;
        s++;
    }

    uint64_t *lat = calloc((size_t)frames, sizeof *lat);
    uint64_t bytes = 0, bytes_i = 0;
    int nkey = 0;
    uint64_t period = 1000000000ull * p.fps_den / p.fps_num;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (int f = 0; f < frames; f++) {
        int k = f % NBUF;
        draw_bgrx(pix, sstride, W, H, (uint32_t)f);
        if (fd) fwrite(pix, 1, (size_t)sstride * H, fd);
        if (bo[k]) {
            uint32_t st;
            void *md = NULL;
            uint8_t *m = gbm_bo_map(bo[k], 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &st, &md);
            if (!m) die("gbm_bo_map");
            for (uint32_t y = 0; y < H; y++) memcpy(m + (size_t)y * st, pix + (size_t)y * sstride, sstride);
            gbm_bo_unmap(bo[k], md);
        } else if (sdpy) {
            VAImage img;
            void *m;
            if (vaDeriveImage(sdpy, ssurf[k], &img) != VA_STATUS_SUCCESS) die("derive image");
            vaMapBuffer(sdpy, img.buf, &m);
            bgrx_to_nv12(pix, sstride, W, H, (uint8_t *)m + img.offsets[0], img.pitches[0],
                         (uint8_t *)m + img.offsets[1], img.pitches[1]);
            vaUnmapBuffer(sdpy, img.buf);
            vaDestroyImage(sdpy, img.image_id);
            vaSyncSurface(sdpy, ssurf[k]);
        } else {
            const void *pl[2] = { pix, NULL };
            uint32_t st[2] = { sstride, 0 };
            if (plh_enc_upload(enc, surf[k], pl, st)) die(plh_enc_last_error(enc));
        }
        if (realtime) {
            next.tv_nsec += (long)period;
            while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
        unsigned flags = 0;
        for (int i = 0; i < nidr; i++) if (idrs[i] == f) flags |= PLH_ENC_FORCE_IDR;
        plh_packet pk;
        if (plh_enc_encode(enc, surf[k], (uint64_t)f, flags, &pk)) {
            fprintf(stderr, "encbench: frame %d: %s\n", f, plh_enc_last_error(enc));
            return 4;
        }
        lat[f] = pk.t_done_ns - pk.t_submit_ns;
        bytes += pk.size;
        if (pk.keyframe) { nkey++; bytes_i += pk.size; }
        if (fo) fwrite(pk.data, 1, pk.size, fo);
        if (getenv("ENCBENCH_VERBOSE"))
            fprintf(stderr, "f=%d key=%d size=%zu lat=%.3fms\n", f, pk.keyframe, pk.size, lat[f] / 1e6);
    }

    /* statistics (excluding frame 0: initialization) */
    int n = frames > 1 ? frames - 1 : frames;
    uint64_t *s = malloc((size_t)n * sizeof *s);
    memcpy(s, lat + (frames - n), (size_t)n * sizeof *s);
    qsort(s, (size_t)n, sizeof *s, cmp_u64);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    double fps = (double)p.fps_num / p.fps_den;
    printf("codec=%s size=%ux%u src=%s rc=%s frames=%d keyframes=%d\n",
           p.codec == PLH_CODEC_H264 ? "h264" : "hevc", W, H, srcdesc,
           p.rc == PLH_RC_CQP ? "cqp" : p.rc == PLH_RC_VBR ? "vbr" : "cbr", frames, nkey);
    printf("latency_ms (excluding frame 0) mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f first=%.3f\n",
           sum / n / 1e6, s[n / 2] / 1e6, s[(int)(n * 0.95)] / 1e6, s[(int)(n * 0.99)] / 1e6,
           s[n - 1] / 1e6, lat[0] / 1e6);
    printf("mean_bitrate=%.2f Mbit/s (target %.2f) bytes=%llu bytes_idr=%llu\n",
           bytes * 8.0 * fps / frames / 1e6, p.bitrate / 1e6,
           (unsigned long long)bytes, (unsigned long long)bytes_i);

    for (int i = 0; i < NBUF; i++) plh_enc_release_surface(enc, surf[i]);
    plh_enc_destroy(enc);
    for (int i = 0; i < NBUF; i++) if (bo[i]) { close(bofd[i]); gbm_bo_destroy(bo[i]); }
    if (gbm) gbm_device_destroy(gbm);
    if (gfd >= 0) close(gfd);
    if (sdpy) { vaDestroySurfaces(sdpy, ssurf, NBUF); vaTerminate(sdpy); close(sfd); }
    if (fo) fclose(fo);
    if (fd) fclose(fd);
    free(pix); free(lat); free(s); free(ybuf); free(uvbuf);
    return 0;
}
