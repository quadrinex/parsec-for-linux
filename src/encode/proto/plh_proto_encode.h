/*
 * plh_proto_encode.h — encode service (0x02xx). Owner: encodeur.
 * Included after plh_proto.h. Compiles with native gcc AND mingw-w64 (sizes checked).
 *
 * Flow (synchronous requests, one session = one stream):
 *   C: ENC_CAPS                      -> D: plh_enc_caps_reply
 *   C: ENC_OPEN  plh_enc_open        -> D: plh_enc_open_reply (shm: input slots + output area)
 *   C: ENC_FRAME plh_enc_frame       -> D: plh_enc_frame_reply (packet in the output area)
 *   C: ENC_RECONFIG plh_enc_reconfig -> D: empty reply (takes effect on the next frame)
 *   C: ENC_CLOSE plh_enc_session_ref -> D: empty reply
 * Closing the connection closes all of the client's sessions.
 *
 * Frame input (plh_enc_frame.source):
 *   PLH_ENC_SRC_SLOT    : pixels written by the client into slot `slot` of the session shm
 *                         (format/stride announced in the request);
 *   PLH_ENC_SRC_CAPTURE : capture frame `capture_frame_id` (PLH_FRAME_ID) held by this same
 *                         client; the daemon encodes the original frame (DMA-BUF if available) without
 *                         going through the client (not implemented yet). The client remains responsible for RELEASE.
 *
 * Shared memory of a session (/dev/shm/<shm_name>):
 *   [0, 4096)                                plh_enc_shm_header header
 *   [in_offset + i*in_slot_size, ...)        input slot i (i < in_slot_count)
 *   [out_offset, out_offset + out_size)      last encoded packet (valid until the next FRAME)
 */
#ifndef PLH_PROTO_ENCODE_H
#define PLH_PROTO_ENCODE_H

#include <stdint.h>

enum plh_msg_type_encode {
    PLH_MSG_ENC_CAPS     = 0x0201, /* C->D empty; reply plh_enc_caps_reply */
    PLH_MSG_ENC_OPEN     = 0x0202, /* C->D plh_enc_open; reply plh_enc_open_reply */
    PLH_MSG_ENC_FRAME    = 0x0203, /* C->D plh_enc_frame; reply plh_enc_frame_reply */
    PLH_MSG_ENC_RECONFIG = 0x0204, /* C->D plh_enc_reconfig; empty reply */
    PLH_MSG_ENC_CLOSE    = 0x0205, /* C->D plh_enc_session_ref; empty reply */
};

enum plh_enc_codec_id { PLH_ENC_CODEC_H264 = 0, PLH_ENC_CODEC_HEVC = 1, PLH_ENC_CODEC_AV1 = 2 };
enum plh_enc_rc_id    { PLH_ENC_RC_CQP = 0, PLH_ENC_RC_CBR = 1, PLH_ENC_RC_VBR = 2 };

struct plh_enc_caps_reply {
    int32_t  status;
    uint32_t codecs;                 /* bit (1 << plh_enc_codec_id) */
    uint32_t h264_max_w, h264_max_h;
    uint32_t hevc_max_w, hevc_max_h;
    uint32_t av1_max_w, av1_max_h;
    uint32_t input_formats;          /* bit (1 << plh_pixel_format) */
    uint32_t reserved;
    char     backend[32];            /* "vaapi" */
    char     device[128];            /* driver vendor string */
};
PLH_STATIC_ASSERT(sizeof(struct plh_enc_caps_reply) == 200, "enc_caps_reply");

struct plh_enc_open {
    uint32_t codec;                  /* plh_enc_codec_id */
    uint32_t profile;                /* profile_idc (66/77/100; HEVC 1); 0 = default */
    uint32_t width, height;          /* encoded size */
    uint32_t fps_num, fps_den;
    uint32_t rc;                     /* plh_enc_rc_id */
    uint32_t bitrate, peak_bitrate, vbv_size;
    uint32_t qp_i, qp_p, min_qp, max_qp;
    uint32_t idr_period;             /* 0 = infini */
    int32_t  quality;                /* -1 default, 0 speed, 1 balanced, 2 quality */
    uint32_t full_range;
    uint32_t in_format;              /* plh_pixel_format of the slots */
    uint32_t in_slot_count;          /* 0 = 2 */
    uint32_t reserved;
};
PLH_STATIC_ASSERT(sizeof(struct plh_enc_open) == 80, "enc_open");

struct plh_enc_open_reply {
    int32_t  status;
    uint32_t session_id;
    uint32_t in_slot_count;
    uint32_t in_stride;              /* recommended stride (Y plane for NV12) */
    uint64_t in_offset;
    uint64_t in_slot_size;
    uint64_t out_offset;
    uint64_t out_size;
    uint64_t shm_size;
    char     shm_name[PLH_NAME_MAX];
    char     backend[32];
};
PLH_STATIC_ASSERT(sizeof(struct plh_enc_open_reply) == 152, "enc_open_reply");

enum plh_enc_source { PLH_ENC_SRC_SLOT = 0, PLH_ENC_SRC_CAPTURE = 1 };

#define PLH_ENC_FRAME_F_FORCE_IDR 0x1

struct plh_enc_frame {
    uint32_t session_id;
    uint32_t source;                 /* plh_enc_source */
    uint32_t slot;
    uint32_t flags;                  /* PLH_ENC_FRAME_F_* */
    uint32_t width, height;          /* actual pixel size of the slot (<= session) */
    uint32_t stride;                 /* bytes per line (Y plane for NV12) */
    uint32_t format;                 /* plh_pixel_format */
    uint64_t uv_offset;              /* NV12: offset of the UV plane in the slot (same stride) */
    uint64_t capture_frame_id;       /* PLH_ENC_SRC_CAPTURE */
    uint64_t pts;                    /* copied unchanged */
};
PLH_STATIC_ASSERT(sizeof(struct plh_enc_frame) == 56, "enc_frame");

struct plh_enc_frame_reply {
    int32_t  status;
    uint32_t keyframe;
    uint64_t frame_index;
    uint64_t pts;
    uint64_t size;                   /* bytes at out_offset */
    uint64_t encode_ns;              /* encode time in the daemon */
    uint64_t upload_ns;              /* slot -> GPU surface copy (0 if zero-copy) */
};
PLH_STATIC_ASSERT(sizeof(struct plh_enc_frame_reply) == 48, "enc_frame_reply");

struct plh_enc_reconfig {
    uint32_t session_id;
    uint32_t bitrate, peak_bitrate;  /* 0 = unchanged */
    uint32_t reserved;
};

struct plh_enc_session_ref {
    uint32_t session_id;
    uint32_t reserved;
};

#define PLH_ENC_SHM_MAGIC 0x434e4548u /* "HENC" */
struct plh_enc_shm_header {
    uint32_t magic, version;
    uint32_t width, height, in_format, in_slot_count;
    uint64_t in_offset, in_slot_size, out_offset, out_size;
};

#endif /* PLH_PROTO_ENCODE_H */
