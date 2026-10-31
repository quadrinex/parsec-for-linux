/* VA-API backend (tested on radeonsi): low-latency H.264 / HEVC.
 *
 * - No B-frames, 1 reference (previous frame), infinite GOP, IDR on demand.
 * - One frame -> one packet, synchronous (vaSyncSurface + vaMapBuffer).
 * - 32-bit RGB inputs converted to NV12 on the GPU (VAProcPipeline, BT.709).
 * - Headers (VPS/SPS/PPS, slice headers) are written by us (bitstream/) and
 *   passed as "packed headers": Mesa 26 radeonsi does not generate them itself
 *   (without them: bare slices, undecodable stream — observed on 2026-10-30).
 *   SPS/PPS repeated at every IDR.
 */
#define _GNU_SOURCE
#include "../plh_enc_internal.h"
#include "../bitstream/plh_hdr.h"

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_vpp.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NREF 2

struct plh_surface {
    VASurfaceID id;
    int         rgb;      /* needs VPP */
    uint32_t    fourcc;
    uint32_t    w, h;
};

typedef struct va_enc {
    struct plh_encoder base;
    int         fd;
    VADisplay   dpy;
    plh_enc_params p;
    VAProfile   profile;
    VAConfigID  cfg, vpp_cfg;
    VAContextID ctx, vpp_ctx;
    VASurfaceID recon[NREF];
    VASurfaceID nv12;          /* cible VPP */
    VABufferID  coded;
    uint32_t    coded_size;
    uint32_t    aw, ah;        /* aligned sizes */
    /* GOP state */
    uint64_t    frame_index;
    uint32_t    frames_since_idr;
    uint32_t    idr_count;
    int         cur;           /* index recon courant */
    int         params_dirty;
    /* output buffer */
    uint8_t    *out;
    size_t      out_cap;
    /* attributs HEVC */
    VAConfigAttribValEncHEVCFeatures   hevc_feat;
    VAConfigAttribValEncHEVCBlockSizes hevc_bs;
    plh_hdr_cfg hdr;
    int         radeonsi;      /* quality_level = Mesa bit field */
} va_enc;

#define E(x) ((va_enc *)(x))
#define VACHK(e, call) do { VAStatus st_ = (call); if (st_ != VA_STATUS_SUCCESS) { \
    plh_enc_seterr(&(e)->base, "%s: %s (%s:%d)", #call, vaErrorStr(st_), __FILE__, __LINE__); \
    goto fail; } } while (0)

static const char *default_device(const char *dev)
{
    if (dev && *dev) return dev;
    const char *env = getenv("PLH_ENC_DEVICE");
    return env && *env ? env : "/dev/dri/renderD128";
}

static void va_silent(void *ctx, const char *msg) { (void)ctx; (void)msg; }

static VADisplay open_display(const char *dev, int *pfd)
{
    int fd = open(default_device(dev), O_RDWR | O_CLOEXEC);
    if (fd < 0) return NULL;
    VADisplay d = vaGetDisplayDRM(fd);
    if (!d) { close(fd); return NULL; }
    vaSetInfoCallback(d, va_silent, NULL);
    int maj, min;
    if (vaInitialize(d, &maj, &min) != VA_STATUS_SUCCESS) {
        vaTerminate(d); close(fd); return NULL;
    }
    *pfd = fd;
    return d;
}

static int has_entry(VADisplay d, VAProfile prof, VAEntrypoint ep)
{
    int n = vaMaxNumEntrypoints(d);
    VAEntrypoint *eps = calloc((size_t)n, sizeof *eps);
    int found = 0;
    if (eps && vaQueryConfigEntrypoints(d, prof, eps, &n) == VA_STATUS_SUCCESS)
        for (int i = 0; i < n; i++) found |= eps[i] == ep;
    free(eps);
    return found;
}

static void max_size(VADisplay d, VAProfile prof, uint32_t *w, uint32_t *h)
{
    VAConfigAttrib a[2] = { { .type = VAConfigAttribMaxPictureWidth },
                            { .type = VAConfigAttribMaxPictureHeight } };
    *w = *h = 0;
    if (vaGetConfigAttributes(d, prof, VAEntrypointEncSlice, a, 2) == VA_STATUS_SUCCESS) {
        if (a[0].value != VA_ATTRIB_NOT_SUPPORTED) *w = a[0].value;
        if (a[1].value != VA_ATTRIB_NOT_SUPPORTED) *h = a[1].value;
    }
}

static int va_query_caps(const char *device, plh_enc_caps *c)
{
    int fd;
    VADisplay d = open_display(device, &fd);
    if (!d) return -1;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "vaapi");
    snprintf(c->device, sizeof c->device, "%s", vaQueryVendorString(d));
    c->h264 = has_entry(d, VAProfileH264High, VAEntrypointEncSlice) ||
              has_entry(d, VAProfileH264ConstrainedBaseline, VAEntrypointEncSlice);
    c->hevc = has_entry(d, VAProfileHEVCMain, VAEntrypointEncSlice);
    c->av1  = has_entry(d, VAProfileAV1Profile0, VAEntrypointEncSlice);
    if (c->h264) max_size(d, VAProfileH264High, &c->h264_max_w, &c->h264_max_h);
    if (c->hevc) max_size(d, VAProfileHEVCMain, &c->hevc_max_w, &c->hevc_max_h);
    c->dmabuf_import = 1;
    c->rgb_input = has_entry(d, VAProfileNone, VAEntrypointVideoProc);
    vaTerminate(d);
    close(fd);
    return 0;
}

/* ---------- niveaux ---------- */

static int h264_level(uint32_t aw, uint32_t ah, double fps)
{
    static const struct { int idc; uint32_t mbps, fs; } t[] = {
        {30, 40500, 1620}, {31, 108000, 3600}, {32, 216000, 5120},
        {40, 245760, 8192}, {41, 245760, 8192}, {42, 522240, 8704},
        {50, 589824, 22080}, {51, 983040, 36864}, {52, 2073600, 36864},
        {60, 4177920, 139264}, {61, 8355840, 139264}, {62, 16711680, 139264},
    };
    uint32_t fs = (aw / 16) * (ah / 16);
    double mbps = fs * fps;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (fs <= t[i].fs && mbps <= t[i].mbps) return t[i].idc;
    return 62;
}

static int hevc_level(uint32_t w, uint32_t h, double fps)
{
    static const struct { int idc; uint64_t ps; double sr; } t[] = {
        {90, 552960, 16588800}, {93, 983040, 33177600},
        {120, 2228224, 66846720}, {123, 2228224, 133693440},
        {150, 8912896, 267386880}, {153, 8912896, 534773760},
        {156, 8912896, 1069547520}, {180, 35651584, 1069547520},
        {183, 35651584, 2139095040}, {186, 35651584, 4278190080.0},
    };
    uint64_t ps = (uint64_t)w * h;
    double sr = ps * fps;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (ps <= t[i].ps && sr <= t[i].sr) return t[i].idc;
    return 186;
}

/* ---------- creation ---------- */

static void va_destroy(plh_encoder *pe)
{
    va_enc *e = E(pe);
    if (!e) return;
    if (e->dpy) {
        if (e->coded != VA_INVALID_ID) vaDestroyBuffer(e->dpy, e->coded);
        if (e->ctx != VA_INVALID_ID) vaDestroyContext(e->dpy, e->ctx);
        if (e->vpp_ctx != VA_INVALID_ID) vaDestroyContext(e->dpy, e->vpp_ctx);
        if (e->cfg != VA_INVALID_ID) vaDestroyConfig(e->dpy, e->cfg);
        if (e->vpp_cfg != VA_INVALID_ID) vaDestroyConfig(e->dpy, e->vpp_cfg);
        for (int i = 0; i < NREF; i++)
            if (e->recon[i] != VA_INVALID_SURFACE) vaDestroySurfaces(e->dpy, &e->recon[i], 1);
        if (e->nv12 != VA_INVALID_SURFACE) vaDestroySurfaces(e->dpy, &e->nv12, 1);
        vaTerminate(e->dpy);
    }
    if (e->fd >= 0) close(e->fd);
    free(e->out);
    free(e);
}

static plh_encoder *va_create(const char *device, const plh_enc_params *pin,
                              char *err, size_t errlen)
{
    va_enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->base.be = &plh_enc_backend_vaapi;
    e->fd = -1;
    e->cfg = e->vpp_cfg = VA_INVALID_ID;
    e->ctx = e->vpp_ctx = VA_INVALID_ID;
    e->coded = VA_INVALID_ID;
    e->nv12 = VA_INVALID_SURFACE;
    for (int i = 0; i < NREF; i++) e->recon[i] = VA_INVALID_SURFACE;
    e->p = *pin;
    plh_enc_params *p = &e->p;
    if (!p->fps_num) { p->fps_num = 60; p->fps_den = 1; }
    if (!p->fps_den) p->fps_den = 1;
    if (!p->qp_i) p->qp_i = 26;
    if (!p->qp_p) p->qp_p = p->qp_i;
    if (p->rc != PLH_RC_CQP && !p->bitrate) p->bitrate = 20000000;
    if (!p->peak_bitrate || p->peak_bitrate < p->bitrate) p->peak_bitrate = p->bitrate;
    if (!p->vbv_size) p->vbv_size = (uint32_t)((uint64_t)p->bitrate * p->fps_den * 4 / p->fps_num);

    e->dpy = open_display(device, &e->fd);
    if (!e->dpy) {
        plh_enc_seterr(&e->base, "vaapi: impossible d'ouvrir %s", default_device(device));
        goto fail;
    }

    {
        const char *vs = vaQueryVendorString(e->dpy);
        e->radeonsi = vs && strstr(vs, "Mesa Gallium") && strstr(vs, "radeonsi");
    }
    if (p->codec == PLH_CODEC_H264) {
        if (!p->profile) p->profile = PLH_PROFILE_H264_HIGH;
        e->profile = p->profile == PLH_PROFILE_H264_CB ? VAProfileH264ConstrainedBaseline :
                     p->profile == PLH_PROFILE_H264_MAIN ? VAProfileH264Main : VAProfileH264High;
        e->aw = (p->width + 15) & ~15u;
        e->ah = (p->height + 15) & ~15u;
    } else {
        p->profile = PLH_PROFILE_HEVC_MAIN;
        e->profile = VAProfileHEVCMain;
        /* surfaces aligned to 16; coded size = visible size (multiple of 8) */
        e->aw = (p->width + 15) & ~15u;
        e->ah = (p->height + 15) & ~15u;
    }
    if (!has_entry(e->dpy, e->profile, VAEntrypointEncSlice)) {
        plh_enc_seterr(&e->base, "vaapi: profile %d not supported for encoding", p->profile);
        goto fail;
    }
    uint32_t mw, mh;
    max_size(e->dpy, e->profile, &mw, &mh);
    if ((mw && p->width > mw) || (mh && p->height > mh)) {
        plh_enc_seterr(&e->base, "vaapi: %ux%u exceeds the driver maximum %ux%u for this codec",
                       p->width, p->height, mw, mh);
        goto fail;
    }

    VAConfigAttrib at[5] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl,
          .value = p->rc == PLH_RC_CBR ? VA_RC_CBR : p->rc == PLH_RC_VBR ? VA_RC_VBR : VA_RC_CQP },
        { .type = VAConfigAttribEncHEVCFeatures },
        { .type = VAConfigAttribEncHEVCBlockSizes },
        { .type = VAConfigAttribEncPackedHeaders },
    };
    {
        VAConfigAttrib q = at[4];
        vaGetConfigAttributes(e->dpy, e->profile, VAEntrypointEncSlice, &q, 1);
        uint32_t want = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE;
        if (q.value == VA_ATTRIB_NOT_SUPPORTED || (q.value & want) != want) {
            plh_enc_seterr(&e->base, "vaapi: packed headers not supported (0x%x)", q.value);
            goto fail;
        }
        at[2] = (VAConfigAttrib){ .type = VAConfigAttribEncPackedHeaders, .value = want };
    }
    int nat = 3;
    if (p->codec == PLH_CODEC_HEVC) {
        VAConfigAttrib q[2] = { { .type = VAConfigAttribEncHEVCFeatures },
                                { .type = VAConfigAttribEncHEVCBlockSizes } };
        vaGetConfigAttributes(e->dpy, e->profile, VAEntrypointEncSlice, q, 2);
        e->hevc_feat.value = q[0].value != VA_ATTRIB_NOT_SUPPORTED ? q[0].value : 0;
        e->hevc_bs.value   = q[1].value != VA_ATTRIB_NOT_SUPPORTED ? q[1].value : 0;
    }
    VACHK(e, vaCreateConfig(e->dpy, e->profile, VAEntrypointEncSlice, at, nat, &e->cfg));

    VASurfaceID tmp[NREF + 1];
    VACHK(e, vaCreateSurfaces(e->dpy, VA_RT_FORMAT_YUV420, e->aw, e->ah, tmp, NREF + 1, NULL, 0));
    for (int i = 0; i < NREF; i++) e->recon[i] = tmp[i];
    e->nv12 = tmp[NREF];
    VACHK(e, vaCreateContext(e->dpy, e->cfg, (int)e->aw, (int)e->ah, VA_PROGRESSIVE,
                             e->recon, NREF, &e->ctx));
    e->coded_size = e->aw * e->ah * 3 / 2 + 65536;
    VACHK(e, vaCreateBuffer(e->dpy, e->ctx, VAEncCodedBufferType, e->coded_size, 1, NULL, &e->coded));

    if (has_entry(e->dpy, VAProfileNone, VAEntrypointVideoProc)) {
        VACHK(e, vaCreateConfig(e->dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &e->vpp_cfg));
        VACHK(e, vaCreateContext(e->dpy, e->vpp_cfg, (int)e->aw, (int)e->ah, VA_PROGRESSIVE,
                                 &e->nv12, 1, &e->vpp_ctx));
    }
    {
        plh_hdr_cfg *h = &e->hdr;
        double fps = (double)p->fps_num / p->fps_den;
        h->codec = p->codec;
        h->profile = p->profile;
        h->width = p->width;
        h->height = p->height;
        h->fps_num = p->fps_num;
        h->fps_den = p->fps_den;
        h->full_range = p->full_range;
        h->init_qp = p->rc == PLH_RC_CQP ? (int)p->qp_i : 26;
        if (p->codec == PLH_CODEC_H264) {
            h->coded_w = e->aw;
            h->coded_h = e->ah;
            h->level_idc = h264_level(e->aw, e->ah, fps);
            h->cabac = p->profile != PLH_PROFILE_H264_CB;
            h->transform_8x8 = p->profile == PLH_PROFILE_H264_HIGH;
        } else {
            uint32_t log2_ctb = e->hevc_bs.value ? 3 + e->hevc_bs.bits.log2_max_coding_tree_block_size_minus3 : 5;
            h->log2_min_cb_minus3 = e->hevc_bs.value ? e->hevc_bs.bits.log2_min_luma_coding_block_size_minus3 : 0;
            uint32_t mincb = 1u << (h->log2_min_cb_minus3 + 3);
            h->coded_w = (p->width + mincb - 1) & ~(mincb - 1);
            h->coded_h = (p->height + mincb - 1) & ~(mincb - 1);
            h->level_idc = hevc_level(h->coded_w, h->coded_h, fps);
            h->log2_diff_max_min_cb = (int)log2_ctb - 3 - h->log2_min_cb_minus3;
            h->log2_min_tb_minus2 = e->hevc_bs.value ? e->hevc_bs.bits.log2_min_luma_transform_block_size_minus2 : 0;
            h->log2_diff_max_min_tb = (e->hevc_bs.value ? e->hevc_bs.bits.log2_max_luma_transform_block_size_minus2 : 3)
                                      - h->log2_min_tb_minus2;
            h->depth_inter = e->hevc_bs.value ? e->hevc_bs.bits.max_max_transform_hierarchy_depth_inter : 3;
            h->depth_intra = e->hevc_bs.value ? e->hevc_bs.bits.max_max_transform_hierarchy_depth_intra : 3;
            h->amp = e->hevc_feat.value ? e->hevc_feat.bits.amp != 0 : 1;
            h->sao = e->hevc_feat.value ? e->hevc_feat.bits.sao != 0 : 0;
            h->strong_intra = e->hevc_feat.value ? e->hevc_feat.bits.strong_intra_smoothing != 0 : 0;
            h->cu_qp_delta = p->rc != PLH_RC_CQP;
        }
    }
    e->params_dirty = 1;
    return &e->base;

fail:
    if (err) snprintf(err, errlen, "%s", e->base.err);
    va_destroy(&e->base);
    return NULL;
}

/* ---------- surfaces ---------- */

static int drm_to_va(uint32_t f, uint32_t *vaf, uint32_t *rt, int *rgb)
{
    *rgb = 1;
    *rt = VA_RT_FORMAT_RGB32;
    switch (f) {
    case DRM_FORMAT_XRGB8888: *vaf = VA_FOURCC_BGRX; return 0;
    case DRM_FORMAT_ARGB8888: *vaf = VA_FOURCC_BGRA; return 0;
    case DRM_FORMAT_XBGR8888: *vaf = VA_FOURCC_RGBX; return 0;
    case DRM_FORMAT_ABGR8888: *vaf = VA_FOURCC_RGBA; return 0;
    case DRM_FORMAT_NV12: *vaf = VA_FOURCC_NV12; *rt = VA_RT_FORMAT_YUV420; *rgb = 0; return 0;
    }
    return -1;
}

static plh_surface *va_import(plh_encoder *pe, const plh_dmabuf *b)
{
    va_enc *e = E(pe);
    uint32_t vaf, rt;
    int rgb;
    if (drm_to_va(b->drm_fourcc, &vaf, &rt, &rgb)) {
        plh_enc_seterr(pe, "vaapi: DRM format 0x%08x not supported", b->drm_fourcc);
        return NULL;
    }
    VADRMPRIMESurfaceDescriptor d;
    memset(&d, 0, sizeof d);
    d.fourcc = vaf;
    d.width = b->width;
    d.height = b->height;
    d.num_layers = 1;
    d.layers[0].drm_format = b->drm_fourcc;
    d.layers[0].num_planes = (uint32_t)b->num_planes;
    for (int i = 0; i < b->num_planes; i++) {
        int obj = -1;
        for (uint32_t j = 0; j < d.num_objects; j++)
            if (d.objects[j].fd == b->fd[i]) obj = (int)j;
        if (obj < 0) {
            obj = (int)d.num_objects++;
            d.objects[obj].fd = b->fd[i];
            off_t sz = b->obj_size[i] ? (off_t)b->obj_size[i] : lseek(b->fd[i], 0, SEEK_END);
            d.objects[obj].size = sz > 0 ? (uint32_t)sz : 0;
            d.objects[obj].drm_format_modifier = b->modifier;
        }
        d.layers[0].object_index[i] = (uint32_t)obj;
        d.layers[0].offset[i] = b->offset[i];
        d.layers[0].pitch[i] = b->pitch[i];
    }
    VASurfaceAttrib sa[2] = {
        { .type = VASurfaceAttribMemoryType, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypeInteger,
                     .value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 } },
        { .type = VASurfaceAttribExternalBufferDescriptor, .flags = VA_SURFACE_ATTRIB_SETTABLE,
          .value = { .type = VAGenericValueTypePointer, .value.p = &d } },
    };
    plh_surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    VAStatus st = vaCreateSurfaces(e->dpy, rt, b->width, b->height, &s->id, 1, sa, 2);
    if (st != VA_STATUS_SUCCESS) {
        plh_enc_seterr(pe, "vaapi: import DMA-BUF (fourcc 0x%08x, mod 0x%llx): %s",
                       b->drm_fourcc, (unsigned long long)b->modifier, vaErrorStr(st));
        free(s);
        return NULL;
    }
    s->rgb = rgb;
    s->fourcc = b->drm_fourcc;
    s->w = b->width;
    s->h = b->height;
    return s;
}

static plh_surface *va_alloc_surface(plh_encoder *pe, uint32_t f)
{
    va_enc *e = E(pe);
    uint32_t vaf, rt;
    int rgb;
    if (drm_to_va(f, &vaf, &rt, &rgb)) {
        plh_enc_seterr(pe, "vaapi: DRM format 0x%08x not supported", f);
        return NULL;
    }
    VASurfaceAttrib sa = { .type = VASurfaceAttribPixelFormat, .flags = VA_SURFACE_ATTRIB_SETTABLE,
                           .value = { .type = VAGenericValueTypeInteger, .value.i = (int)vaf } };
    plh_surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    VAStatus st = vaCreateSurfaces(e->dpy, rt, e->p.width, e->p.height, &s->id, 1, &sa, 1);
    if (st != VA_STATUS_SUCCESS) {
        plh_enc_seterr(pe, "vaapi: surface allocation: %s", vaErrorStr(st));
        free(s);
        return NULL;
    }
    s->rgb = rgb;
    s->fourcc = f;
    s->w = e->p.width;
    s->h = e->p.height;
    return s;
}

static int va_upload(plh_encoder *pe, plh_surface *s, const void *const pl[2],
                     const uint32_t st[2])
{
    va_enc *e = E(pe);
    VAImage img;
    void *ptr = NULL;
    VACHK(e, vaDeriveImage(e->dpy, s->id, &img));
    if (vaMapBuffer(e->dpy, img.buf, &ptr) != VA_STATUS_SUCCESS) {
        vaDestroyImage(e->dpy, img.image_id);
        plh_enc_seterr(pe, "vaapi: vaMapBuffer(image) failed");
        return -1;
    }
    uint32_t bpp = s->rgb ? 4 : 1;
    for (uint32_t y = 0; y < s->h; y++)
        memcpy((uint8_t *)ptr + img.offsets[0] + (size_t)y * img.pitches[0],
               (const uint8_t *)pl[0] + (size_t)y * st[0], (size_t)s->w * bpp);
    if (!s->rgb)
        for (uint32_t y = 0; y < s->h / 2; y++)
            memcpy((uint8_t *)ptr + img.offsets[1] + (size_t)y * img.pitches[1],
                   (const uint8_t *)pl[1] + (size_t)y * st[1], s->w);
    vaUnmapBuffer(e->dpy, img.buf);
    vaDestroyImage(e->dpy, img.image_id);
    return 0;
fail:
    return -1;
}

static void va_release(plh_encoder *pe, plh_surface *s)
{
    vaDestroySurfaces(E(pe)->dpy, &s->id, 1);
    free(s);
}

/* ---------- encodage ---------- */

typedef struct { VABufferID ids[24]; int n; } buflist;

static int add_buf(va_enc *e, buflist *bl, VABufferType t, size_t sz, void *data)
{
    if (bl->n >= 24) return -1;
    VACHK(e, vaCreateBuffer(e->dpy, e->ctx, t, (unsigned)sz, 1, data, &bl->ids[bl->n]));
    bl->n++;
    return 0;
fail:
    return -1;
}

static int add_misc(va_enc *e, buflist *bl, VAEncMiscParameterType type, const void *payload, size_t sz)
{
    uint8_t buf[sizeof(VAEncMiscParameterBuffer) + 256];
    if (sz > 256) return -1;
    memset(buf, 0, sizeof buf);
    VAEncMiscParameterBuffer *m = (VAEncMiscParameterBuffer *)buf;
    m->type = type;
    memcpy(m->data, payload, sz);
    return add_buf(e, bl, VAEncMiscParameterBufferType, sizeof *m + sz, buf);
}

static int add_packed(va_enc *e, buflist *bl, uint32_t type, const uint8_t *data, uint32_t bits)
{
    VAEncPackedHeaderParameterBuffer ph = { .type = type, .bit_length = bits, .has_emulation_bytes = 1 };
    if (add_buf(e, bl, VAEncPackedHeaderParameterBufferType, sizeof ph, &ph)) return -1;
    return add_buf(e, bl, VAEncPackedHeaderDataBufferType, (bits + 7) / 8, (void *)data);
}

static int add_seq_header(va_enc *e, buflist *bl)
{
    uint8_t buf[512];
    size_t n = plh_hdr_sequence(&e->hdr, buf, sizeof buf);
    if (!n) { plh_enc_seterr(&e->base, "sequence header too long"); return -1; }
    return add_packed(e, bl, VAEncPackedHeaderSequence, buf, (uint32_t)n * 8);
}

static int add_slice_header(va_enc *e, buflist *bl, int idr, uint32_t frame_num, int32_t poc, int qpd)
{
    uint8_t buf[128];
    uint32_t bits = 0;
    size_t n = plh_hdr_slice(&e->hdr, idr, frame_num, poc, e->idr_count, qpd, buf, sizeof buf, &bits);
    if (!n) { plh_enc_seterr(&e->base, "slice header too long"); return -1; }
    return add_packed(e, bl, VAEncPackedHeaderSlice, buf, bits);
}

static int add_rc_params(va_enc *e, buflist *bl)
{
    const plh_enc_params *p = &e->p;
    VAEncMiscParameterFrameRate fr = { .framerate = (p->fps_den << 16) | p->fps_num };
    if (add_misc(e, bl, VAEncMiscParameterTypeFrameRate, &fr, sizeof fr)) return -1;
    if (p->rc != PLH_RC_CQP) {
        VAEncMiscParameterRateControl rc;
        memset(&rc, 0, sizeof rc);
        rc.bits_per_second = p->rc == PLH_RC_VBR ? p->peak_bitrate : p->bitrate;
        rc.target_percentage = p->rc == PLH_RC_VBR
            ? (uint32_t)((uint64_t)p->bitrate * 100 / p->peak_bitrate) : 100;
        rc.window_size = 1000;
        rc.initial_qp = 0;
        rc.min_qp = p->min_qp;
        rc.max_qp = p->max_qp;
        if (add_misc(e, bl, VAEncMiscParameterTypeRateControl, &rc, sizeof rc)) return -1;
        VAEncMiscParameterHRD hrd = { .initial_buffer_fullness = p->vbv_size * 3 / 4,
                                      .buffer_size = p->vbv_size };
        if (add_misc(e, bl, VAEncMiscParameterTypeHRD, &hrd, sizeof hrd)) return -1;
    }
    if (p->quality >= 0) {
        /* 0 = speed, 1 = balanced, 2 = quality.
         * Mesa radeonsi reads quality_level as a bit field (vlVaQualityBits:
         * bit0 valid_setting, bit1 vbaq_mode, bits2-3 preset_mode 0=speed 1=balance
         * 2=quality); other VA drivers: 1 = best quality ... 7 = fastest. */
        int qv = p->quality > 2 ? 2 : p->quality;
        uint32_t lvl = e->radeonsi ? (1u | ((uint32_t)qv << 2)) : (uint32_t)(qv == 2 ? 1 : qv == 1 ? 4 : 7);
        VAEncMiscParameterBufferQualityLevel q = { .quality_level = lvl };
        if (add_misc(e, bl, VAEncMiscParameterTypeQualityLevel, &q, sizeof q)) return -1;
    }
    return 0;
}

static int build_h264(va_enc *e, buflist *bl, VASurfaceID src, int idr)
{
    (void)src;
    const plh_enc_params *p = &e->p;
    uint32_t mbw = e->aw / 16, mbh = e->ah / 16;
    VASurfaceID cur = e->recon[e->cur], ref = e->recon[e->cur ^ 1];
    uint32_t frame_num = e->frames_since_idr & 0xffff;     /* log2_max_frame_num = 16 */
    int32_t poc = (int32_t)(e->frames_since_idr * 2);   /* full POC; lsb in the header */
    double fps = (double)p->fps_num / p->fps_den;

    if (idr) {
        VAEncSequenceParameterBufferH264 sq;
        memset(&sq, 0, sizeof sq);
        sq.seq_parameter_set_id = 0;
        sq.level_idc = (uint8_t)h264_level(e->aw, e->ah, fps);
        sq.intra_period = p->idr_period;
        sq.intra_idr_period = p->idr_period;
        sq.ip_period = 1;
        sq.bits_per_second = p->rc == PLH_RC_CQP ? 0 : (p->rc == PLH_RC_VBR ? p->peak_bitrate : p->bitrate);
        sq.max_num_ref_frames = 1;
        sq.picture_width_in_mbs = (uint16_t)mbw;
        sq.picture_height_in_mbs = (uint16_t)mbh;
        sq.seq_fields.bits.chroma_format_idc = 1;
        sq.seq_fields.bits.frame_mbs_only_flag = 1;
        sq.seq_fields.bits.direct_8x8_inference_flag = 1;
        sq.seq_fields.bits.log2_max_frame_num_minus4 = 12;
        sq.seq_fields.bits.pic_order_cnt_type = 0;
        sq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 12;
        if (e->aw != p->width || e->ah != p->height) {
            sq.frame_cropping_flag = 1;
            sq.frame_crop_right_offset = (e->aw - p->width) / 2;
            sq.frame_crop_bottom_offset = (e->ah - p->height) / 2;
        }
        sq.vui_parameters_present_flag = 1;
        sq.vui_fields.bits.aspect_ratio_info_present_flag = 0;
        sq.vui_fields.bits.timing_info_present_flag = 1;
        sq.vui_fields.bits.fixed_frame_rate_flag = 1;
        sq.vui_fields.bits.bitstream_restriction_flag = 1;
        sq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        sq.vui_fields.bits.log2_max_mv_length_vertical = 15;
        sq.num_units_in_tick = p->fps_den;
        sq.time_scale = 2 * p->fps_num;
        if (add_buf(e, bl, VAEncSequenceParameterBufferType, sizeof sq, &sq)) return -1;
    }
    if (idr || e->params_dirty) {
        if (add_rc_params(e, bl)) return -1;
        e->params_dirty = 0;
    }
    if (idr && add_seq_header(e, bl)) return -1;

    VAEncPictureParameterBufferH264 pp;
    memset(&pp, 0, sizeof pp);
    pp.CurrPic.picture_id = cur;
    pp.CurrPic.frame_idx = frame_num;
    pp.CurrPic.flags = 0;
    pp.CurrPic.TopFieldOrderCnt = poc;
    pp.CurrPic.BottomFieldOrderCnt = poc;
    for (int i = 0; i < 16; i++) {
        pp.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pp.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    VAPictureH264 refpic;
    memset(&refpic, 0, sizeof refpic);
    if (!idr) {
        refpic.picture_id = ref;
        refpic.frame_idx = (frame_num - 1) & 0xffff;
        refpic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        refpic.TopFieldOrderCnt = refpic.BottomFieldOrderCnt = poc - 2;
        pp.ReferenceFrames[0] = refpic;
    }
    pp.coded_buf = e->coded;
    pp.pic_parameter_set_id = 0;
    pp.seq_parameter_set_id = 0;
    pp.frame_num = (uint16_t)frame_num;
    pp.pic_init_qp = (uint8_t)(p->rc == PLH_RC_CQP ? p->qp_i : 26);
    pp.num_ref_idx_l0_active_minus1 = 0;
    pp.pic_fields.bits.idr_pic_flag = idr;
    pp.pic_fields.bits.reference_pic_flag = 1;
    pp.pic_fields.bits.entropy_coding_mode_flag = p->profile != PLH_PROFILE_H264_CB;
    pp.pic_fields.bits.transform_8x8_mode_flag = p->profile == PLH_PROFILE_H264_HIGH;
    pp.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    if (add_buf(e, bl, VAEncPictureParameterBufferType, sizeof pp, &pp)) return -1;

    VAEncSliceParameterBufferH264 sl;
    memset(&sl, 0, sizeof sl);
    sl.macroblock_address = 0;
    sl.num_macroblocks = mbw * mbh;
    sl.macroblock_info = VA_INVALID_ID;
    sl.slice_type = idr ? 2 : 0;   /* I : P */
    sl.pic_parameter_set_id = 0;
    sl.idr_pic_id = (uint16_t)e->idr_count;
    sl.pic_order_cnt_lsb = (uint16_t)poc;
    sl.direct_spatial_mv_pred_flag = 1;
    sl.num_ref_idx_l0_active_minus1 = 0;
    for (int i = 0; i < 32; i++) {
        sl.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
        sl.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        sl.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
        sl.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
    }
    if (!idr) sl.RefPicList0[0] = refpic;
    sl.slice_qp_delta = (int8_t)(p->rc == PLH_RC_CQP ? (int)(idr ? p->qp_i : p->qp_p) - (int)p->qp_i : 0);
    if (add_slice_header(e, bl, idr, frame_num, poc, sl.slice_qp_delta)) return -1;
    if (add_buf(e, bl, VAEncSliceParameterBufferType, sizeof sl, &sl)) return -1;
    return 0;
}

static int build_hevc(va_enc *e, buflist *bl, int idr)
{
    const plh_enc_params *p = &e->p;
    VASurfaceID cur = e->recon[e->cur], ref = e->recon[e->cur ^ 1];
    int32_t poc = (int32_t)e->frames_since_idr;   /* full POC; lsb in the header */
    double fps = (double)p->fps_num / p->fps_den;
    uint32_t log2_ctb = e->hevc_bs.value ? 3 + e->hevc_bs.bits.log2_max_coding_tree_block_size_minus3 : 5;
    uint32_t ctb = 1u << log2_ctb;
    uint32_t pw = e->hdr.coded_w, ph = e->hdr.coded_h;
    uint32_t nctu = ((pw + ctb - 1) / ctb) * ((ph + ctb - 1) / ctb);
    int sao = e->hevc_feat.value ? e->hevc_feat.bits.sao != 0 : 0;

    if (idr) {
        VAEncSequenceParameterBufferHEVC sq;
        memset(&sq, 0, sizeof sq);
        sq.general_profile_idc = 1;
        sq.general_level_idc = (uint8_t)hevc_level(pw, ph, fps);
        sq.general_tier_flag = 0;
        sq.intra_period = p->idr_period;
        sq.intra_idr_period = p->idr_period;
        sq.ip_period = 1;
        sq.bits_per_second = p->rc == PLH_RC_CQP ? 0 : (p->rc == PLH_RC_VBR ? p->peak_bitrate : p->bitrate);
        sq.pic_width_in_luma_samples = (uint16_t)pw;
        sq.pic_height_in_luma_samples = (uint16_t)ph;
        sq.seq_fields.bits.chroma_format_idc = 1;
        sq.seq_fields.bits.amp_enabled_flag = e->hevc_feat.value ? e->hevc_feat.bits.amp != 0 : 1;
        sq.seq_fields.bits.sample_adaptive_offset_enabled_flag = sao;
        sq.seq_fields.bits.strong_intra_smoothing_enabled_flag =
            e->hevc_feat.value ? e->hevc_feat.bits.strong_intra_smoothing != 0 : 0;
        sq.seq_fields.bits.sps_temporal_mvp_enabled_flag = 0;
        sq.seq_fields.bits.low_delay_seq = 1;
        sq.log2_min_luma_coding_block_size_minus3 =
            e->hevc_bs.value ? e->hevc_bs.bits.log2_min_luma_coding_block_size_minus3 : 0;
        sq.log2_diff_max_min_luma_coding_block_size =
            (uint8_t)(log2_ctb - 3 - sq.log2_min_luma_coding_block_size_minus3);
        sq.log2_min_transform_block_size_minus2 =
            e->hevc_bs.value ? e->hevc_bs.bits.log2_min_luma_transform_block_size_minus2 : 0;
        sq.log2_diff_max_min_transform_block_size = (uint8_t)
            ((e->hevc_bs.value ? e->hevc_bs.bits.log2_max_luma_transform_block_size_minus2 : 3)
             - sq.log2_min_transform_block_size_minus2);
        sq.max_transform_hierarchy_depth_inter =
            e->hevc_bs.value ? e->hevc_bs.bits.max_max_transform_hierarchy_depth_inter : 3;
        sq.max_transform_hierarchy_depth_intra =
            e->hevc_bs.value ? e->hevc_bs.bits.max_max_transform_hierarchy_depth_intra : 3;
        sq.vui_parameters_present_flag = 1;
        sq.vui_fields.bits.vui_timing_info_present_flag = 1;
        sq.vui_fields.bits.bitstream_restriction_flag = 1;
        sq.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
        sq.vui_fields.bits.restricted_ref_pic_lists_flag = 1;
        sq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
        sq.vui_fields.bits.log2_max_mv_length_vertical = 15;
        sq.vui_num_units_in_tick = p->fps_den;
        sq.vui_time_scale = p->fps_num;
        if (add_buf(e, bl, VAEncSequenceParameterBufferType, sizeof sq, &sq)) return -1;
    }
    if (idr || e->params_dirty) {
        if (add_rc_params(e, bl)) return -1;
        e->params_dirty = 0;
    }
    if (idr && add_seq_header(e, bl)) return -1;

    VAEncPictureParameterBufferHEVC pp;
    memset(&pp, 0, sizeof pp);
    pp.decoded_curr_pic.picture_id = cur;
    pp.decoded_curr_pic.pic_order_cnt = poc;
    pp.decoded_curr_pic.flags = 0;
    for (int i = 0; i < 15; i++) {
        pp.reference_frames[i].picture_id = VA_INVALID_SURFACE;
        pp.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    VAPictureHEVC refpic = { .picture_id = ref, .pic_order_cnt = poc - 1, .flags = 0 };
    if (!idr) pp.reference_frames[0] = refpic;
    pp.coded_buf = e->coded;
    pp.collocated_ref_pic_index = 0xff;
    pp.last_picture = 0;
    pp.pic_init_qp = (uint8_t)(p->rc == PLH_RC_CQP ? p->qp_i : 26);
    pp.diff_cu_qp_delta_depth = 0;
    pp.num_ref_idx_l0_default_active_minus1 = 0;
    pp.slice_pic_parameter_set_id = 0;
    pp.nal_unit_type = idr ? 19 /* IDR_W_RADL */ : 1 /* TRAIL_R */;
    pp.pic_fields.bits.idr_pic_flag = idr;
    pp.pic_fields.bits.coding_type = idr ? 1 : 2;
    pp.pic_fields.bits.reference_pic_flag = 1;
    pp.pic_fields.bits.cu_qp_delta_enabled_flag = p->rc != PLH_RC_CQP;
    pp.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    if (add_buf(e, bl, VAEncPictureParameterBufferType, sizeof pp, &pp)) return -1;

    VAEncSliceParameterBufferHEVC sl;
    memset(&sl, 0, sizeof sl);
    sl.slice_segment_address = 0;
    sl.num_ctu_in_slice = nctu;
    sl.slice_type = idr ? 2 : 1;   /* I : P */
    sl.slice_pic_parameter_set_id = 0;
    sl.num_ref_idx_l0_active_minus1 = 0;
    for (int i = 0; i < 15; i++) {
        sl.ref_pic_list0[i].picture_id = VA_INVALID_SURFACE;
        sl.ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
        sl.ref_pic_list1[i].picture_id = VA_INVALID_SURFACE;
        sl.ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    if (!idr) sl.ref_pic_list0[0] = refpic;
    sl.max_num_merge_cand = 5;
    sl.slice_qp_delta = (int8_t)(p->rc == PLH_RC_CQP ? (int)(idr ? p->qp_i : p->qp_p) - (int)p->qp_i : 0);
    sl.slice_fields.bits.last_slice_of_pic_flag = 1;
    sl.slice_fields.bits.slice_sao_luma_flag = sao;
    sl.slice_fields.bits.slice_sao_chroma_flag = sao;
    sl.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
    if (add_slice_header(e, bl, idr, 0, poc, sl.slice_qp_delta)) return -1;
    if (add_buf(e, bl, VAEncSliceParameterBufferType, sizeof sl, &sl)) return -1;
    return 0;
}

static int run_vpp(va_enc *e, plh_surface *in)
{
    if (e->vpp_ctx == VA_INVALID_ID) {
        plh_enc_seterr(&e->base, "vaapi: no VideoProc to convert RGB->NV12");
        return -1;
    }
    VAProcPipelineParameterBuffer pl;
    memset(&pl, 0, sizeof pl);
    VARectangle rin = { 0, 0, (uint16_t)in->w, (uint16_t)in->h };
    VARectangle rout = { 0, 0, (uint16_t)e->p.width, (uint16_t)e->p.height };
    pl.surface = in->id;
    pl.surface_region = &rin;
    pl.output_region = &rout;
    pl.output_background_color = 0xff000000;
    pl.surface_color_standard = VAProcColorStandardBT709;
    pl.output_color_standard = VAProcColorStandardBT709;
    pl.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    pl.output_color_properties.color_range = e->p.full_range ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
    VABufferID b = VA_INVALID_ID;
    VACHK(e, vaCreateBuffer(e->dpy, e->vpp_ctx, VAProcPipelineParameterBufferType, sizeof pl, 1, &pl, &b));
    VACHK(e, vaBeginPicture(e->dpy, e->vpp_ctx, e->nv12));
    VACHK(e, vaRenderPicture(e->dpy, e->vpp_ctx, &b, 1));
    VACHK(e, vaEndPicture(e->dpy, e->vpp_ctx));
    vaDestroyBuffer(e->dpy, b);
    return 0;
fail:
    if (b != VA_INVALID_ID) vaDestroyBuffer(e->dpy, b);
    return -1;
}

static int va_encode(plh_encoder *pe, plh_surface *in, uint64_t pts, unsigned flags,
                     plh_packet *out)
{
    va_enc *e = E(pe);
    buflist bl = { .n = 0 };
    void *seg_ptr = NULL;
    int began = 0;
    uint64_t t0 = plh_enc_now_ns();
    int idr = e->frame_index == 0 || (flags & PLH_ENC_FORCE_IDR) ||
              (e->p.idr_period && e->frames_since_idr >= e->p.idr_period);
    if (idr) e->frames_since_idr = 0;

    VASurfaceID src = in->id;
    if (in->rgb) {
        if (run_vpp(e, in)) return -1;
        src = e->nv12;
    }

    int r = e->p.codec == PLH_CODEC_H264 ? build_h264(e, &bl, src, idr) : build_hevc(e, &bl, idr);
    if (r) goto fail;

    VACHK(e, vaBeginPicture(e->dpy, e->ctx, src));
    began = 1;
    VACHK(e, vaRenderPicture(e->dpy, e->ctx, bl.ids, bl.n));
    began = 0;
    VACHK(e, vaEndPicture(e->dpy, e->ctx));
    VACHK(e, vaSyncSurface(e->dpy, src));
    VACHK(e, vaMapBuffer(e->dpy, e->coded, &seg_ptr));

    size_t total = 0;
    for (VACodedBufferSegment *s = seg_ptr; s; s = s->next) total += s->size;
    if (total > e->out_cap) {
        uint8_t *n = realloc(e->out, total);
        if (!n) { vaUnmapBuffer(e->dpy, e->coded); plh_enc_seterr(pe, "oom"); goto fail; }
        e->out = n;
        e->out_cap = total;
    }
    size_t off = 0;
    for (VACodedBufferSegment *s = seg_ptr; s; s = s->next) {
        memcpy(e->out + off, s->buf, s->size);
        off += s->size;
    }
    vaUnmapBuffer(e->dpy, e->coded);
    for (int i = 0; i < bl.n; i++) vaDestroyBuffer(e->dpy, bl.ids[i]);

    out->data = e->out;
    out->size = total;
    out->keyframe = idr;
    out->pts = pts;
    out->frame_index = e->frame_index;
    out->t_submit_ns = t0;
    out->t_done_ns = plh_enc_now_ns();

    if (idr) e->idr_count++;
    e->frame_index++;
    e->frames_since_idr++;
    e->cur ^= 1;
    return 0;

fail:
    (void)began;
    for (int i = 0; i < bl.n; i++) vaDestroyBuffer(e->dpy, bl.ids[i]);
    return -1;
}

static int va_set_bitrate(plh_encoder *pe, uint32_t br, uint32_t peak)
{
    va_enc *e = E(pe);
    if (!br) return -1;
    e->p.bitrate = br;
    e->p.peak_bitrate = peak >= br ? peak : br;
    e->p.vbv_size = (uint32_t)((uint64_t)br * e->p.fps_den * 4 / e->p.fps_num);
    e->params_dirty = 1;
    return 0;
}

const struct plh_enc_backend plh_enc_backend_vaapi = {
    .name = "vaapi",
    .query_caps = va_query_caps,
    .create = va_create,
    .destroy = va_destroy,
    .import_dmabuf = va_import,
    .alloc_surface = va_alloc_surface,
    .upload = va_upload,
    .release_surface = va_release,
    .encode = va_encode,
    .set_bitrate = va_set_bitrate,
};
