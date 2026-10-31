/* Encode service (0x02) of the plh-bridge daemon. Owner: encodeur.
 * One session = one plh_encoder + one shm (input slots + output area).
 * Synchronous in the daemon loop: ~3-4 ms per 1080p frame (measured, N1). */
#define _GNU_SOURCE
#include "plh_bridge.h"
#include "../proto/plh_proto_encode.h"
#include "plh_enc.h"

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "encode"
#define MAX_SESSIONS 4

struct enc_session {
    uint32_t      id;
    plh_encoder  *enc;
    plh_enc_params p;
    struct plh_shm shm;
    uint32_t      in_format, in_stride, in_slot_count;
    uint64_t      in_offset, in_slot_size, out_offset, out_size;
    plh_surface  *surf_rgb, *surf_nv12;
    uint32_t      surf_rgb_fourcc;
};

struct enc_client {
    struct enc_session *s[MAX_SESSIONS];
};

static uint32_t next_session_id = 1;
static plh_enc_caps g_caps;

static uint32_t drm_of(uint32_t f)
{
    switch (f) {
    case PLH_FMT_BGRA8: return DRM_FORMAT_ARGB8888;
    case PLH_FMT_BGRX8: return DRM_FORMAT_XRGB8888;
    case PLH_FMT_RGBA8: return DRM_FORMAT_ABGR8888;
    case PLH_FMT_RGBX8: return DRM_FORMAT_XBGR8888;
    case PLH_FMT_NV12:  return DRM_FORMAT_NV12;
    }
    return 0;
}

static void session_free(struct enc_session *s)
{
    if (!s) return;
    if (s->enc) {
        plh_enc_release_surface(s->enc, s->surf_rgb);
        plh_enc_release_surface(s->enc, s->surf_nv12);
        plh_enc_destroy(s->enc);
    }
    if (s->shm.map) plh_shm_destroy(&s->shm);
    free(s);
}

static struct enc_client *get_client(struct plh_client *c, int create)
{
    struct enc_client *ec = plh_client_get_data(c, PLH_SVC_ENCODE);
    if (!ec && create) {
        ec = calloc(1, sizeof *ec);
        plh_client_set_data(c, PLH_SVC_ENCODE, ec);
    }
    return ec;
}

static struct enc_session **find(struct plh_client *c, uint32_t id)
{
    struct enc_client *ec = get_client(c, 0);
    if (!ec) return NULL;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (ec->s[i] && ec->s[i]->id == id) return &ec->s[i];
    return NULL;
}

static void do_caps(struct plh_client *c, const struct plh_msg_hdr *h)
{
    struct plh_enc_caps_reply r;
    memset(&r, 0, sizeof r);
    r.codecs = (g_caps.h264 ? 1u << PLH_ENC_CODEC_H264 : 0) | (g_caps.hevc ? 1u << PLH_ENC_CODEC_HEVC : 0) |
               (g_caps.av1 ? 1u << PLH_ENC_CODEC_AV1 : 0);
    r.h264_max_w = g_caps.h264_max_w; r.h264_max_h = g_caps.h264_max_h;
    r.hevc_max_w = g_caps.hevc_max_w; r.hevc_max_h = g_caps.hevc_max_h;
    r.input_formats = (1u << PLH_FMT_BGRA8) | (1u << PLH_FMT_BGRX8) | (1u << PLH_FMT_RGBA8) |
                      (1u << PLH_FMT_RGBX8) | (1u << PLH_FMT_NV12);
    snprintf(r.backend, sizeof r.backend, "%s", g_caps.name);
    snprintf(r.device, sizeof r.device, "%s", g_caps.device);
    plh_reply(c, h, &r, sizeof r);
}

static void do_open(struct plh_client *c, const struct plh_msg_hdr *h, const struct plh_enc_open *o)
{
    struct enc_client *ec = get_client(c, 1);
    int idx = -1;
    for (int i = 0; ec && i < MAX_SESSIONS; i++) if (!ec->s[i]) { idx = i; break; }
    if (idx < 0) { plh_reply_error(c, h, PLH_E_BUSY, "too many encode sessions"); return; }
    if (!o->width || !o->height || o->width > 8192 || o->height > 8192 || !drm_of(o->in_format)) {
        plh_reply_error(c, h, PLH_E_INVAL, "invalid parameters (%ux%u, format %u)", o->width, o->height, o->in_format);
        return;
    }
    struct enc_session *s = calloc(1, sizeof *s);
    if (!s) { plh_reply_error(c, h, PLH_E_NOMEM, "oom"); return; }
    plh_enc_params *p = &s->p;
    p->codec = o->codec == PLH_ENC_CODEC_HEVC ? PLH_CODEC_HEVC : PLH_CODEC_H264;
    p->profile = (int)o->profile;
    p->width = o->width; p->height = o->height;
    p->fps_num = o->fps_num ? o->fps_num : 60; p->fps_den = o->fps_den ? o->fps_den : 1;
    p->rc = o->rc == PLH_ENC_RC_CQP ? PLH_RC_CQP : o->rc == PLH_ENC_RC_VBR ? PLH_RC_VBR : PLH_RC_CBR;
    p->bitrate = o->bitrate; p->peak_bitrate = o->peak_bitrate; p->vbv_size = o->vbv_size;
    p->qp_i = o->qp_i; p->qp_p = o->qp_p; p->min_qp = o->min_qp; p->max_qp = o->max_qp;
    p->idr_period = o->idr_period;
    p->quality = o->quality;
    p->full_range = (int)o->full_range;
    if (o->codec == PLH_ENC_CODEC_AV1) { free(s); plh_reply_error(c, h, PLH_E_INVAL, "AV1 not supported by this GPU"); return; }

    char err[256];
    s->enc = plh_enc_create(NULL, NULL, p, err, sizeof err);
    if (!s->enc) { free(s); plh_reply_error(c, h, PLH_E_INVAL, "%s", err); return; }

    s->in_format = o->in_format;
    s->in_slot_count = o->in_slot_count ? (o->in_slot_count > 4 ? 4 : o->in_slot_count) : 2;
    uint32_t bpp = o->in_format == PLH_FMT_NV12 ? 1 : 4;
    s->in_stride = (o->width * bpp + 255) & ~255u;
    uint64_t slot = (uint64_t)s->in_stride * o->height;
    if (o->in_format == PLH_FMT_NV12) slot += (uint64_t)s->in_stride * ((o->height + 1) / 2);
    /* a slot can also receive BGRA at this size (texture readback) */
    uint64_t slot_rgb = (uint64_t)((o->width * 4 + 255) & ~255u) * o->height;
    if (slot_rgb > slot) slot = slot_rgb;
    s->in_slot_size = (slot + 4095) & ~4095ull;
    s->in_offset = 4096;
    s->out_offset = s->in_offset + s->in_slot_size * s->in_slot_count;
    s->out_size = (((uint64_t)o->width * o->height * 3 / 2 + 65536) + 4095) & ~4095ull;
    if (plh_shm_create(&s->shm, "enc", s->out_offset + s->out_size) < 0) {
        session_free(s);
        plh_reply_error(c, h, PLH_E_IO, "shm");
        return;
    }
    struct plh_enc_shm_header *sh = s->shm.map;
    sh->magic = PLH_ENC_SHM_MAGIC; sh->version = 1;
    sh->width = o->width; sh->height = o->height; sh->in_format = o->in_format;
    sh->in_slot_count = s->in_slot_count;
    sh->in_offset = s->in_offset; sh->in_slot_size = s->in_slot_size;
    sh->out_offset = s->out_offset; sh->out_size = s->out_size;

    s->id = next_session_id++;
    ec->s[idx] = s;

    struct plh_enc_open_reply r;
    memset(&r, 0, sizeof r);
    r.session_id = s->id;
    r.in_slot_count = s->in_slot_count;
    r.in_stride = s->in_stride;
    r.in_offset = s->in_offset;
    r.in_slot_size = s->in_slot_size;
    r.out_offset = s->out_offset;
    r.out_size = s->out_size;
    r.shm_size = s->shm.size;
    snprintf(r.shm_name, sizeof r.shm_name, "%s", s->shm.name);
    snprintf(r.backend, sizeof r.backend, "%s", plh_enc_backend_name(s->enc));
    PLH_INFO(MOD, "session %u: %s %ux%u @%u/%u rc=%u %u bit/s, input fmt %u, client %u",
             s->id, p->codec == PLH_CODEC_HEVC ? "hevc" : "h264", p->width, p->height,
             p->fps_num, p->fps_den, o->rc, p->bitrate, o->in_format, plh_client_id(c));
    plh_reply(c, h, &r, sizeof r);
}

static void do_frame(struct plh_client *c, const struct plh_msg_hdr *h, const struct plh_enc_frame *f)
{
    struct enc_session **ps = find(c, f->session_id), *s;
    if (!ps) { plh_reply_error(c, h, PLH_E_NOENT, "session %u inconnue", f->session_id); return; }
    s = *ps;
    if (f->source == PLH_ENC_SRC_CAPTURE) {
        /* Not implemented: resolve the capture frame_id to a DMA-BUF (to be wired up with capture) */
        plh_reply_error(c, h, PLH_E_UNKNOWN, "capture source not available yet");
        return;
    }
    if (f->slot >= s->in_slot_count || f->width != s->p.width || f->height != s->p.height) {
        plh_reply_error(c, h, PLH_E_INVAL, "invalid slot %u / size %ux%u (session %ux%u)",
                        f->slot, f->width, f->height, s->p.width, s->p.height);
        return;
    }
    uint32_t fourcc = drm_of(f->format);
    int nv12 = f->format == PLH_FMT_NV12;
    uint64_t need = nv12 ? f->uv_offset + (uint64_t)f->stride * ((f->height + 1) / 2)
                         : (uint64_t)f->stride * f->height;
    if (!fourcc || need > s->in_slot_size || f->stride < f->width * (nv12 ? 1 : 4) ||
        (nv12 && f->uv_offset < (uint64_t)f->stride * f->height)) {
        plh_reply_error(c, h, PLH_E_INVAL, "format/stride invalides");
        return;
    }
    plh_surface **surf = nv12 ? &s->surf_nv12 : &s->surf_rgb;
    if (!nv12 && *surf && s->surf_rgb_fourcc != fourcc) {
        plh_enc_release_surface(s->enc, *surf);
        *surf = NULL;
    }
    if (!*surf) {
        *surf = plh_enc_alloc_surface(s->enc, fourcc);
        if (!*surf) { plh_reply_error(c, h, PLH_E_IO, "%s", plh_enc_last_error(s->enc)); return; }
        if (!nv12) s->surf_rgb_fourcc = fourcc;
    }
    const uint8_t *base = (const uint8_t *)s->shm.map + s->in_offset + f->slot * s->in_slot_size;
    const void *planes[2] = { base, nv12 ? base + f->uv_offset : NULL };
    const uint32_t strides[2] = { f->stride, nv12 ? f->stride : 0 };
    uint64_t t0 = plh_now_ns();
    if (plh_enc_upload(s->enc, *surf, planes, strides)) {
        plh_reply_error(c, h, PLH_E_IO, "%s", plh_enc_last_error(s->enc));
        return;
    }
    uint64_t t1 = plh_now_ns();
    plh_packet pk;
    if (plh_enc_encode(s->enc, *surf, f->pts, (f->flags & PLH_ENC_FRAME_F_FORCE_IDR) ? PLH_ENC_FORCE_IDR : 0, &pk)) {
        plh_reply_error(c, h, PLH_E_IO, "%s", plh_enc_last_error(s->enc));
        return;
    }
    if (pk.size > s->out_size) {
        plh_reply_error(c, h, PLH_E_NOMEM, "paquet %zu > zone %llu", pk.size, (unsigned long long)s->out_size);
        return;
    }
    memcpy((uint8_t *)s->shm.map + s->out_offset, pk.data, pk.size);
    struct plh_enc_frame_reply r = {
        .status = PLH_OK, .keyframe = (uint32_t)pk.keyframe, .frame_index = pk.frame_index,
        .pts = pk.pts, .size = pk.size, .encode_ns = pk.t_done_ns - pk.t_submit_ns, .upload_ns = t1 - t0,
    };
    plh_reply(c, h, &r, sizeof r);
}

static void handle(struct plh_client *c, const struct plh_msg_hdr *h, const void *p)
{
    switch (h->type) {
    case PLH_MSG_ENC_CAPS:
        do_caps(c, h);
        return;
    case PLH_MSG_ENC_OPEN:
        if (h->size < sizeof(struct plh_enc_open)) break;
        do_open(c, h, p);
        return;
    case PLH_MSG_ENC_FRAME:
        if (h->size < sizeof(struct plh_enc_frame)) break;
        do_frame(c, h, p);
        return;
    case PLH_MSG_ENC_RECONFIG: {
        if (h->size < sizeof(struct plh_enc_reconfig)) break;
        const struct plh_enc_reconfig *r = p;
        struct enc_session **ps = find(c, r->session_id);
        if (!ps) { plh_reply_error(c, h, PLH_E_NOENT, "session %u inconnue", r->session_id); return; }
        if (r->bitrate && plh_enc_set_bitrate((*ps)->enc, r->bitrate, r->peak_bitrate)) {
            plh_reply_error(c, h, PLH_E_INVAL, "%s", plh_enc_last_error((*ps)->enc));
            return;
        }
        plh_reply(c, h, NULL, 0);
        return;
    }
    case PLH_MSG_ENC_CLOSE: {
        if (h->size < sizeof(struct plh_enc_session_ref)) break;
        const struct plh_enc_session_ref *r = p;
        struct enc_session **ps = find(c, r->session_id);
        if (!ps) { plh_reply_error(c, h, PLH_E_NOENT, "session %u inconnue", r->session_id); return; }
        session_free(*ps);
        *ps = NULL;
        plh_reply(c, h, NULL, 0);
        return;
    }
    default:
        if (h->seq) plh_reply_error(c, h, PLH_E_UNKNOWN, "message encodage 0x%04x inconnu", h->type);
        return;
    }
    if (h->seq) plh_reply_error(c, h, PLH_E_PROTO, "charge utile trop courte");
}

static void client_gone(struct plh_client *c)
{
    struct enc_client *ec = get_client(c, 0);
    if (!ec) return;
    for (int i = 0; i < MAX_SESSIONS; i++) session_free(ec->s[i]);
    free(ec);
    plh_client_set_data(c, PLH_SVC_ENCODE, NULL);
}

static int init(void)
{
    if (plh_enc_query_caps(NULL, NULL, &g_caps)) {
        PLH_WARN(MOD, "aucun backend d'encodage disponible");
        return -1;
    }
    PLH_INFO(MOD, "backend %s : %s (h264 %d max %ux%u, hevc %d max %ux%u)", g_caps.name, g_caps.device,
             g_caps.h264, g_caps.h264_max_w, g_caps.h264_max_h, g_caps.hevc, g_caps.hevc_max_w, g_caps.hevc_max_h);
    return 0;
}

static const struct plh_service enc_svc = {
    .name = "encode", .id = PLH_SVC_ENCODE, .init = init, .handle = handle, .client_gone = client_gone,
};
PLH_SERVICE_REGISTER(enc_svc)
