/* plh_enc.h — hardware/software video encoder abstraction (Linux native side).
 *
 * One encoder = one stream (H.264 or HEVC), low latency:
 *   - no B-frames, one input frame -> one packet (synchronous),
 *   - infinite GOP by default (idr_period = 0), IDR on demand (PLH_ENC_FORCE_IDR),
 *   - CQP / CBR / VBR, bitrate changeable on the fly.
 * Inputs: DMA-BUF (NV12 or 32-bit RGB, RGB->NV12 conversion on the GPU) or
 * CPU memory (fallback: texture readback, software backend).
 *
 * Backends: "vaapi", "nvenc" (probe only, untested), "software" (x264). Not implemented: "vulkan".
 */
#ifndef PLH_ENC_H
#define PLH_ENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PLH_CODEC_H264 = 0, PLH_CODEC_HEVC = 1 } plh_codec;

/* Profiles: standard profile_idc values. */
enum {
    PLH_PROFILE_H264_CB   = 66,  /* Constrained Baseline */
    PLH_PROFILE_H264_MAIN = 77,
    PLH_PROFILE_H264_HIGH = 100,
    PLH_PROFILE_HEVC_MAIN = 1,
};

typedef enum { PLH_RC_CQP = 0, PLH_RC_CBR = 1, PLH_RC_VBR = 2 } plh_rc_mode;

typedef struct plh_enc_params {
    plh_codec   codec;
    int         profile;        /* 0 = default (High / Main) */
    uint32_t    width, height;  /* visible size */
    uint32_t    fps_num, fps_den;
    plh_rc_mode rc;
    uint32_t    bitrate;        /* bit/s (CBR/VBR : cible) */
    uint32_t    peak_bitrate;   /* bit/s (VBR: peak), 0 = bitrate */
    uint32_t    vbv_size;       /* bits, 0 = bitrate/fps * 4 (faible latence) */
    uint32_t    qp_i, qp_p;     /* CQP ; 0 = 26 */
    uint32_t    min_qp, max_qp; /* 0 = driver default */
    uint32_t    idr_period;     /* 0 = infinite (IDR only on demand) */
    int         quality;        /* -1 = default; 0 = max speed ... 2 = quality */
    int         full_range;     /* 0 = limited range (default), 1 = full */
} plh_enc_params;

/* Input formats = DRM fourcc codes (drm_fourcc.h). Supported:
 * DRM_FORMAT_NV12, DRM_FORMAT_XRGB8888 (BGRX in memory), ARGB8888,
 * XBGR8888, ABGR8888. */
typedef struct plh_dmabuf {
    uint32_t drm_fourcc;
    uint32_t width, height;
    uint64_t modifier;          /* DRM_FORMAT_MOD_LINEAR, ... */
    int      num_planes;
    int      fd[4];             /* the caller keeps ownership of the fds */
    uint32_t offset[4];
    uint32_t pitch[4];
    uint64_t obj_size[4];       /* 0 = inconnu */
} plh_dmabuf;

typedef struct plh_packet {
    uint8_t *data;              /* owned by the encoder, valid until the next call */
    size_t   size;
    int      keyframe;          /* 1 if IDR */
    uint64_t pts;
    uint64_t frame_index;
    /* mesures (ns, CLOCK_MONOTONIC) */
    uint64_t t_submit_ns, t_done_ns;
} plh_packet;

#define PLH_ENC_FORCE_IDR 0x1u

typedef struct plh_encoder plh_encoder;
typedef struct plh_surface plh_surface;   /* imported input, reusable */

typedef struct plh_enc_caps {
    char     name[32];
    char     device[128];
    int      h264, hevc, av1;
    uint32_t h264_max_w, h264_max_h;
    uint32_t hevc_max_w, hevc_max_h;
    int      dmabuf_import, rgb_input;
} plh_enc_caps;

/* backend = NULL or "auto": first available backend.
 * device: DRM node (NULL = /dev/dri/renderD128 or $PLH_ENC_DEVICE). */
int  plh_enc_query_caps(const char *backend, const char *device, plh_enc_caps *caps);
plh_encoder *plh_enc_create(const char *backend, const char *device,
                            const plh_enc_params *p, char *err, size_t errlen);
void plh_enc_destroy(plh_encoder *e);

/* Imports a DMA-BUF as an input surface (to be cached by the caller:
 * capture buffers are recycled). */
plh_surface *plh_enc_import_dmabuf(plh_encoder *e, const plh_dmabuf *buf);
/* CPU-memory input surface (NV12 or 32-bit RGB format); upload with
 * plh_enc_upload before each encode. */
plh_surface *plh_enc_alloc_surface(plh_encoder *e, uint32_t drm_fourcc);
int  plh_enc_upload(plh_encoder *e, plh_surface *s, const void *const planes[2],
                    const uint32_t strides[2]);
void plh_enc_release_surface(plh_encoder *e, plh_surface *s);

/* Encodes one frame; synchronous: *out holds the complete packet on return.
 * 0 = ok, <0 = error. */
int  plh_enc_encode(plh_encoder *e, plh_surface *in, uint64_t pts,
                    unsigned flags, plh_packet *out);

/* Bitrate change on the fly (takes effect on the next frame). */
int  plh_enc_set_bitrate(plh_encoder *e, uint32_t bitrate, uint32_t peak_bitrate);

const char *plh_enc_backend_name(const plh_encoder *e);
const char *plh_enc_last_error(const plh_encoder *e);

#ifdef __cplusplus
}
#endif
#endif
