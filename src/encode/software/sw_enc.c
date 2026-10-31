/* Last-resort software backend: x264 loaded with dlopen (no hard
 * dependency). H.264 only; CPU inputs (NV12, XRGB/ARGB/XBGR/ABGR 32 bits). */
#define _POSIX_C_SOURCE 200809L
#include "../plh_enc_internal.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x264.h>   /* types only: symbols come from dlopen */

#define FOURCC(a,b,c,d) ((uint32_t)(a)|(uint32_t)(b)<<8|(uint32_t)(c)<<16|(uint32_t)(d)<<24)
#define F_NV12 FOURCC('N','V','1','2')
#define F_XR24 FOURCC('X','R','2','4')
#define F_AR24 FOURCC('A','R','2','4')
#define F_XB24 FOURCC('X','B','2','4')
#define F_AB24 FOURCC('A','B','2','4')

#define STR2(x) #x
#define STR(x) STR2(x)

struct x264_api {
    void *h;
    void (*param_default_preset)(x264_param_t *, const char *, const char *);
    int  (*param_apply_profile)(x264_param_t *, const char *);
    x264_t *(*encoder_open)(x264_param_t *);
    int  (*encoder_encode)(x264_t *, x264_nal_t **, int *, x264_picture_t *, x264_picture_t *);
    void (*encoder_close)(x264_t *);
    int  (*encoder_reconfig)(x264_t *, x264_param_t *);
    void (*picture_init)(x264_picture_t *);
};
static struct x264_api X;

static int load_x264(void)
{
    if (X.h) return 0;
    const char *names[] = { "libx264.so." STR(X264_BUILD), "libx264.so", NULL };
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
#define SYM(f, n) if (!(X.f = dlsym(h, n))) { dlclose(h); return -1; }
    SYM(param_default_preset, "x264_param_default_preset")
    SYM(param_apply_profile, "x264_param_apply_profile")
    SYM(encoder_open, "x264_encoder_open_" STR(X264_BUILD))
    SYM(encoder_encode, "x264_encoder_encode")
    SYM(encoder_close, "x264_encoder_close")
    SYM(encoder_reconfig, "x264_encoder_reconfig")
    SYM(picture_init, "x264_picture_init")
#undef SYM
    X.h = h;
    return 0;
}

struct plh_surface { uint32_t fourcc, w, h; uint8_t *data[2]; uint32_t stride[2]; };

typedef struct {
    struct plh_encoder base;
    plh_enc_params p;
    x264_t *x;
    uint8_t *out; size_t out_cap;
    x264_param_t xparam;
    uint8_t *nv12; uint32_t nv12_stride;
    uint64_t frame_index;
} sw_enc;
#define E(pe) ((sw_enc *)(pe))

static int sw_query_caps(const char *device, plh_enc_caps *c)
{
    (void)device;
    if (load_x264()) return -1;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "software");
    snprintf(c->device, sizeof c->device, "x264 build %d (dlopen)", X264_BUILD);
    c->h264 = 1; c->h264_max_w = c->h264_max_h = 8192;
    c->dmabuf_import = 0; c->rgb_input = 1;
    return 0;
}

static void sw_destroy(plh_encoder *pe)
{
    sw_enc *e = E(pe);
    if (e->x) X.encoder_close(e->x);
    free(e->out); free(e->nv12); free(e);
}

static plh_encoder *sw_create(const char *device, const plh_enc_params *pin, char *err, size_t errlen)
{
    (void)device;
    if (load_x264()) { snprintf(err, errlen, "software: libx264 not found"); return NULL; }
    if (pin->codec != PLH_CODEC_H264) { snprintf(err, errlen, "software: HEVC not supported"); return NULL; }
    if ((pin->width & 1) || (pin->height & 1) || !pin->width || !pin->height) {
        snprintf(err, errlen, "software: even dimensions required"); return NULL;
    }
    sw_enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->base.be = &plh_enc_backend_software;
    e->p = *pin;
    plh_enc_params *p = &e->p;
    if (!p->fps_num) { p->fps_num = 60; p->fps_den = 1; }
    if (!p->fps_den) p->fps_den = 1;
    if (!p->qp_i) p->qp_i = 26;
    if (p->rc != PLH_RC_CQP && !p->bitrate) p->bitrate = 20000000;
    if (!p->peak_bitrate || p->peak_bitrate < p->bitrate) p->peak_bitrate = p->bitrate;
    if (!p->vbv_size) p->vbv_size = (uint32_t)((uint64_t)p->bitrate * p->fps_den * 4 / p->fps_num);

    x264_param_t xp;
    X.param_default_preset(&xp, "ultrafast", "zerolatency");
    xp.i_csp = X264_CSP_NV12;
    xp.i_width = (int)p->width; xp.i_height = (int)p->height;
    xp.i_fps_num = p->fps_num; xp.i_fps_den = p->fps_den;
    xp.i_timebase_num = 1; xp.i_timebase_den = 1000000;
    xp.i_bframe = 0; xp.b_repeat_headers = 1; xp.b_annexb = 1; xp.b_intra_refresh = 0;
    xp.i_keyint_max = p->idr_period ? (int)p->idr_period : X264_KEYINT_MAX_INFINITE;
    xp.i_keyint_min = 1; xp.i_scenecut_threshold = 0;
    xp.b_vfr_input = 0;
    xp.vui.b_fullrange = p->full_range;
    xp.vui.i_colmatrix = 1; xp.vui.i_colorprim = 1; xp.vui.i_transfer = 1;
    xp.b_aud = 0;
    xp.i_log_level = X264_LOG_NONE;
    if (p->rc == PLH_RC_CQP) {
        xp.rc.i_rc_method = X264_RC_CQP; xp.rc.i_qp_constant = (int)p->qp_i;
    } else {
        xp.rc.i_rc_method = X264_RC_ABR;
        xp.rc.i_bitrate = (int)(p->bitrate / 1000);
        xp.rc.i_vbv_max_bitrate = (int)(p->peak_bitrate / 1000);
        xp.rc.i_vbv_buffer_size = (int)(p->vbv_size / 1000);
        if (p->rc == PLH_RC_CBR) { xp.rc.i_vbv_max_bitrate = xp.rc.i_bitrate; xp.rc.b_filler = 0; }
        if (xp.rc.i_vbv_buffer_size < 1) xp.rc.i_vbv_buffer_size = 1;
    }
    if (p->min_qp) xp.rc.i_qp_min = (int)p->min_qp;
    if (p->max_qp) xp.rc.i_qp_max = (int)p->max_qp;
    const char *prof = p->profile == PLH_PROFILE_H264_CB ? "baseline" :
                       p->profile == PLH_PROFILE_H264_MAIN ? "main" : "high";
    if (X.param_apply_profile(&xp, prof)) { snprintf(err, errlen, "software: profile %s rejected", prof); free(e); return NULL; }
    e->xparam = xp;
    e->x = X.encoder_open(&xp);
    if (!e->x) { snprintf(err, errlen, "software: x264_encoder_open failed"); free(e); return NULL; }
    return &e->base;
}

static int fmt_ok(uint32_t f) { return f == F_NV12 || f == F_XR24 || f == F_AR24 || f == F_XB24 || f == F_AB24; }

static plh_surface *sw_alloc(plh_encoder *pe, uint32_t f)
{
    sw_enc *e = E(pe);
    if (!fmt_ok(f)) { plh_enc_seterr(pe, "software: DRM format 0x%08x not supported", f); return NULL; }
    plh_surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->fourcc = f; s->w = e->p.width; s->h = e->p.height;
    if (f == F_NV12) {
        s->stride[0] = s->stride[1] = s->w;
        s->data[0] = malloc((size_t)s->w * s->h);
        s->data[1] = malloc((size_t)s->w * s->h / 2);
    } else {
        s->stride[0] = s->w * 4;
        s->data[0] = malloc((size_t)s->w * 4 * s->h);
    }
    if (!s->data[0] || (f == F_NV12 && !s->data[1])) { free(s->data[0]); free(s->data[1]); free(s); return NULL; }
    return s;
}

static int sw_upload(plh_encoder *pe, plh_surface *s, const void *const pl[2], const uint32_t st[2])
{
    (void)pe;
    uint32_t rb = s->fourcc == F_NV12 ? s->w : s->w * 4;
    for (uint32_t y = 0; y < s->h; y++)
        memcpy(s->data[0] + (size_t)y * s->stride[0], (const uint8_t *)pl[0] + (size_t)y * st[0], rb);
    if (s->fourcc == F_NV12)
        for (uint32_t y = 0; y < s->h / 2; y++)
            memcpy(s->data[1] + (size_t)y * s->stride[1], (const uint8_t *)pl[1] + (size_t)y * st[1], s->w);
    return 0;
}

static void sw_release(plh_encoder *pe, plh_surface *s)
{
    (void)pe;
    free(s->data[0]); free(s->data[1]); free(s);
}

/* 32-bit RGB -> NV12 BT.709, limited (or full) range. r,g,b offsets within the 4-byte word. */
static void rgb_to_nv12(const plh_surface *s, int ro, int go, int bo, int full,
                        uint8_t *y, uint32_t ys, uint8_t *uv, uint32_t uvs)
{
    for (uint32_t j = 0; j < s->h; j += 2) {
        for (uint32_t i = 0; i < s->w; i += 2) {
            int su = 0, sv = 0;
            for (int dj = 0; dj < 2; dj++)
                for (int di = 0; di < 2; di++) {
                    const uint8_t *px = s->data[0] + (size_t)(j + dj) * s->stride[0] + (size_t)(i + di) * 4;
                    int r = px[ro], g = px[go], b = px[bo];
                    int yy = full ? (54 * r + 183 * g + 19 * b + 128) >> 8
                                  : ((47 * r + 157 * g + 16 * b + 128) >> 8) + 16;
                    y[(size_t)(j + dj) * ys + i + di] = (uint8_t)yy;
                    int cb, cr;
                    if (full) { cb = (-29 * r - 99 * g + 128 * b) >> 8; cr = (128 * r - 116 * g - 12 * b) >> 8; }
                    else      { cb = (-26 * r - 87 * g + 112 * b) >> 8; cr = (112 * r - 102 * g - 10 * b) >> 8; }
                    su += cb; sv += cr;
                }
            int u = (su + 2) / 4 + 128, v = (sv + 2) / 4 + 128;
            uv[(size_t)(j / 2) * uvs + i] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
            uv[(size_t)(j / 2) * uvs + i + 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

static int sw_encode(plh_encoder *pe, plh_surface *in, uint64_t pts, unsigned flags, plh_packet *out)
{
    sw_enc *e = E(pe);
    uint64_t t0 = plh_enc_now_ns();
    x264_picture_t pic, pout;
    X.picture_init(&pic);
    pic.i_pts = (int64_t)pts;
    pic.img.i_csp = X264_CSP_NV12;
    if (in->fourcc == F_NV12) {
        pic.img.i_plane = 2;
        pic.img.plane[0] = in->data[0]; pic.img.i_stride[0] = (int)in->stride[0];
        pic.img.plane[1] = in->data[1]; pic.img.i_stride[1] = (int)in->stride[1];
    } else {
        if (!e->nv12) {
            e->nv12_stride = e->p.width;
            e->nv12 = malloc((size_t)e->nv12_stride * e->p.height * 3 / 2);
            if (!e->nv12) { plh_enc_seterr(pe, "software: out of memory"); return -1; }
        }
        uint8_t *y = e->nv12, *uv = e->nv12 + (size_t)e->nv12_stride * e->p.height;
        /* bytes in memory (little-endian): XRGB8888 = B,G,R,X; XBGR8888 = R,G,B,X */
        int bgr = in->fourcc == F_XR24 || in->fourcc == F_AR24;
        rgb_to_nv12(in, bgr ? 2 : 0, 1, bgr ? 0 : 2, e->p.full_range, y, e->nv12_stride, uv, e->nv12_stride);
        pic.img.i_plane = 2;
        pic.img.plane[0] = y;  pic.img.i_stride[0] = (int)e->nv12_stride;
        pic.img.plane[1] = uv; pic.img.i_stride[1] = (int)e->nv12_stride;
    }
    int idr = e->frame_index == 0 || (flags & PLH_ENC_FORCE_IDR);
    pic.i_type = idr ? X264_TYPE_IDR : X264_TYPE_AUTO;
    x264_nal_t *nal; int nn = 0;
    int sz = X.encoder_encode(e->x, &nal, &nn, &pic, &pout);
    if (sz < 0) { plh_enc_seterr(pe, "software: x264_encoder_encode failed"); return -1; }
    if (sz == 0) { plh_enc_seterr(pe, "software: no output (unexpected internal delay)"); return -1; }
    if ((size_t)sz > e->out_cap) {
        uint8_t *n = realloc(e->out, (size_t)sz);
        if (!n) return -1;
        e->out = n; e->out_cap = (size_t)sz;
    }
    memcpy(e->out, nal[0].p_payload, (size_t)sz);
    out->data = e->out; out->size = (size_t)sz;
    out->keyframe = pout.b_keyframe ? 1 : 0;
    out->pts = pts; out->frame_index = e->frame_index++;
    out->t_submit_ns = t0; out->t_done_ns = plh_enc_now_ns();
    return 0;
}

static int sw_set_bitrate(plh_encoder *pe, uint32_t br, uint32_t peak)
{
    sw_enc *e = E(pe);
    if (e->p.rc == PLH_RC_CQP) { plh_enc_seterr(pe, "software: CQP has no bitrate"); return -1; }
    if (!peak || peak < br) peak = br;
    e->p.bitrate = br; e->p.peak_bitrate = peak;
    e->xparam.rc.i_bitrate = (int)(br / 1000);
    e->xparam.rc.i_vbv_max_bitrate = (int)(e->p.rc == PLH_RC_CBR ? br / 1000 : peak / 1000);
    if (X.encoder_reconfig(e->x, &e->xparam) < 0) { plh_enc_seterr(pe, "software: reconfig rejected"); return -1; }
    return 0;
}

const struct plh_enc_backend plh_enc_backend_software = {
    .name = "software",
    .query_caps = sw_query_caps, .create = sw_create, .destroy = sw_destroy,
    .import_dmabuf = NULL, .alloc_surface = sw_alloc, .upload = sw_upload,
    .release_surface = sw_release, .encode = sw_encode, .set_bitrate = sw_set_bitrate,
};
