/* H.264 / HEVC headers — see plh_hdr.h. References: ITU-T H.264 §7.3, H.265 §7.3. */
#include "plh_hdr.h"

#include <string.h>

typedef struct { uint8_t b[512]; size_t bits; int ovf; } bw;

static void put(bw *w, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (w->bits >= sizeof w->b * 8) { w->ovf = 1; return; }
        size_t byte = w->bits >> 3;
        if ((w->bits & 7) == 0) w->b[byte] = 0;
        if ((v >> i) & 1) w->b[byte] |= (uint8_t)(0x80 >> (w->bits & 7));
        w->bits++;
    }
}
static void u1(bw *w, int v) { put(w, v ? 1 : 0, 1); }
static void ue(bw *w, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1;
    int len = 0;
    while ((x >> len) > 1) len++;
    put(w, 0, len);
    put(w, (uint32_t)x, len + 1);
}
static void se(bw *w, int v) { ue(w, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v)); }
static void trailing(bw *w) { u1(w, 1); while (w->bits & 7) u1(w, 0); }

/* Copy with start code + emulation prevention. Returns bytes written. */
static size_t emit(const bw *w, uint8_t *out, size_t cap, uint32_t *bits)
{
    size_t nbytes = (w->bits + 7) / 8, o = 0;
    int zeros = 0;
    if (w->ovf || cap < 4) return 0;
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1;
    for (size_t i = 0; i < nbytes; i++) {
        if (zeros >= 2 && w->b[i] <= 3) {
            if (o >= cap) return 0;
            out[o++] = 3;
            zeros = 0;
        }
        if (o >= cap) return 0;
        out[o++] = w->b[i];
        zeros = w->b[i] == 0 ? zeros + 1 : 0;
    }
    if (bits) {
        uint32_t pad = (uint32_t)(nbytes * 8 - w->bits);
        *bits = (uint32_t)(o * 8 - pad);
    }
    return o;
}

/* ---------------- H.264 ---------------- */

static void h264_nal(bw *w, int ref_idc, int type) { put(w, 0, 1); put(w, (uint32_t)ref_idc, 2); put(w, (uint32_t)type, 5); }

static void h264_sps(const plh_hdr_cfg *c, bw *w)
{
    h264_nal(w, 3, 7);
    put(w, (uint32_t)c->profile, 8);
    int cs0 = c->profile == PLH_PROFILE_H264_CB, cs1 = c->profile != PLH_PROFILE_H264_HIGH;
    u1(w, cs0); u1(w, cs1); u1(w, 0); u1(w, 0); u1(w, 0); u1(w, 0); put(w, 0, 2);
    put(w, (uint32_t)c->level_idc, 8);
    ue(w, 0);                                   /* seq_parameter_set_id */
    if (c->profile == PLH_PROFILE_H264_HIGH) {
        ue(w, 1); ue(w, 0); ue(w, 0); u1(w, 0); u1(w, 0);
    }
    ue(w, PLH_H264_LOG2_MAX_FRAME_NUM - 4);
    ue(w, 0);                                   /* pic_order_cnt_type */
    ue(w, PLH_H264_LOG2_MAX_POC_LSB - 4);
    ue(w, 1);                                   /* max_num_ref_frames */
    u1(w, 0);                                   /* gaps_in_frame_num */
    ue(w, c->coded_w / 16 - 1);
    ue(w, c->coded_h / 16 - 1);
    u1(w, 1);                                   /* frame_mbs_only */
    u1(w, 1);                                   /* direct_8x8_inference */
    int crop = c->coded_w != c->width || c->coded_h != c->height;
    u1(w, crop);
    if (crop) { ue(w, 0); ue(w, (c->coded_w - c->width) / 2); ue(w, 0); ue(w, (c->coded_h - c->height) / 2); }
    u1(w, 1);                                   /* vui_parameters_present */
    u1(w, 0);                                   /* aspect_ratio_info */
    u1(w, 0);                                   /* overscan_info */
    u1(w, 1);                                   /* video_signal_type */
    put(w, 5, 3); u1(w, c->full_range); u1(w, 1);
    put(w, 1, 8); put(w, 1, 8); put(w, 1, 8);   /* BT.709 */
    u1(w, 0);                                   /* chroma_loc_info */
    u1(w, 1);                                   /* timing_info */
    put(w, c->fps_den, 32); put(w, 2 * c->fps_num, 32); u1(w, 1);
    u1(w, 0); u1(w, 0);                         /* nal/vcl hrd */
    u1(w, 0);                                   /* pic_struct_present */
    u1(w, 1);                                   /* bitstream_restriction */
    u1(w, 1); ue(w, 0); ue(w, 0); ue(w, 15); ue(w, 15);
    ue(w, 0);                                   /* max_num_reorder_frames */
    ue(w, 1);                                   /* max_dec_frame_buffering */
    trailing(w);
}

static void h264_pps(const plh_hdr_cfg *c, bw *w)
{
    h264_nal(w, 3, 8);
    ue(w, 0); ue(w, 0);
    u1(w, c->cabac);
    u1(w, 0);                                   /* bottom_field_pic_order */
    ue(w, 0);                                   /* num_slice_groups_minus1 */
    ue(w, 0); ue(w, 0);                         /* num_ref_idx_default */
    u1(w, 0); put(w, 0, 2);                     /* weighted */
    se(w, c->init_qp - 26);
    se(w, 0); se(w, 0);
    u1(w, 1);                                   /* deblocking_filter_control_present */
    u1(w, 0); u1(w, 0);
    if (c->profile == PLH_PROFILE_H264_HIGH) {
        u1(w, c->transform_8x8); u1(w, 0); se(w, 0);
    }
    trailing(w);
}

/* ---------------- HEVC ---------------- */

static void hevc_nal(bw *w, int type) { put(w, 0, 1); put(w, (uint32_t)type, 6); put(w, 0, 6); put(w, 1, 3); }

static void hevc_ptl(const plh_hdr_cfg *c, bw *w)
{
    put(w, 0, 2); u1(w, 0); put(w, 1, 5);       /* space, tier, Main */
    put(w, 0x60000000u, 32);                    /* compatibility: Main + Main10 */
    u1(w, 1); u1(w, 0); u1(w, 0); u1(w, 1);     /* progressive, !interlaced, !non_packed, frame_only */
    put(w, 0, 32); put(w, 0, 12);               /* 43 reserved + inbld */
    put(w, (uint32_t)c->level_idc, 8);
}

static void hevc_vps(const plh_hdr_cfg *c, bw *w)
{
    hevc_nal(w, 32);
    put(w, 0, 4); u1(w, 1); u1(w, 1); put(w, 0, 6); put(w, 0, 3); u1(w, 1);
    put(w, 0xffff, 16);
    hevc_ptl(c, w);
    u1(w, 0);                                   /* sub_layer_ordering_info_present */
    ue(w, 1); ue(w, 0); ue(w, 0);
    put(w, 0, 6);                               /* vps_max_layer_id */
    ue(w, 0);                                   /* num_layer_sets_minus1 */
    u1(w, 1);                                   /* timing_info */
    put(w, c->fps_den, 32); put(w, c->fps_num, 32); u1(w, 0); ue(w, 0);
    u1(w, 0);                                   /* extension */
    trailing(w);
}

static void hevc_sps(const plh_hdr_cfg *c, bw *w)
{
    hevc_nal(w, 33);
    put(w, 0, 4); put(w, 0, 3); u1(w, 1);
    hevc_ptl(c, w);
    ue(w, 0);                                   /* sps_id */
    ue(w, 1);                                   /* 4:2:0 */
    ue(w, c->coded_w); ue(w, c->coded_h);
    int crop = c->coded_w != c->width || c->coded_h != c->height;
    u1(w, crop);
    if (crop) { ue(w, 0); ue(w, (c->coded_w - c->width) / 2); ue(w, 0); ue(w, (c->coded_h - c->height) / 2); }
    ue(w, 0); ue(w, 0);                         /* bit depths */
    ue(w, PLH_HEVC_LOG2_MAX_POC_LSB - 4);
    u1(w, 0); ue(w, 1); ue(w, 0); ue(w, 0);     /* ordering info */
    ue(w, (uint32_t)c->log2_min_cb_minus3); ue(w, (uint32_t)c->log2_diff_max_min_cb);
    ue(w, (uint32_t)c->log2_min_tb_minus2); ue(w, (uint32_t)c->log2_diff_max_min_tb);
    ue(w, (uint32_t)c->depth_inter); ue(w, (uint32_t)c->depth_intra);
    u1(w, 0);                                   /* scaling_list */
    u1(w, c->amp); u1(w, c->sao);
    u1(w, 0);                                   /* pcm */
    ue(w, 1);                                   /* num_short_term_ref_pic_sets */
    ue(w, 1); ue(w, 0); ue(w, 0); u1(w, 1);     /* RPS 0: {-1} used */
    u1(w, 0);                                   /* long_term_ref_pics_present */
    u1(w, 0);                                   /* temporal_mvp */
    u1(w, c->strong_intra);
    u1(w, 1);                                   /* vui */
    u1(w, 0); u1(w, 0);
    u1(w, 1); put(w, 5, 3); u1(w, c->full_range); u1(w, 1);
    put(w, 1, 8); put(w, 1, 8); put(w, 1, 8);
    u1(w, 0); u1(w, 0); u1(w, 0); u1(w, 0); u1(w, 0);
    u1(w, 1); put(w, c->fps_den, 32); put(w, c->fps_num, 32); u1(w, 0); u1(w, 0);
    u1(w, 1);                                   /* bitstream_restriction */
    u1(w, 0); u1(w, 1); u1(w, 1); ue(w, 0); ue(w, 0); ue(w, 0); ue(w, 15); ue(w, 15);
    u1(w, 0);                                   /* sps_extension */
    trailing(w);
}

static void hevc_pps(const plh_hdr_cfg *c, bw *w)
{
    hevc_nal(w, 34);
    ue(w, 0); ue(w, 0);
    u1(w, 0); u1(w, 0); put(w, 0, 3); u1(w, 0); u1(w, 0);
    ue(w, 0); ue(w, 0);
    se(w, c->init_qp - 26);
    u1(w, 0); u1(w, 0);
    u1(w, c->cu_qp_delta);
    if (c->cu_qp_delta) ue(w, 0);
    se(w, 0); se(w, 0);
    u1(w, 0); u1(w, 0); u1(w, 0); u1(w, 0); u1(w, 0); u1(w, 0);
    u1(w, 1);                                   /* loop_filter_across_slices */
    u1(w, 0);                                   /* deblocking_filter_control_present */
    u1(w, 0); u1(w, 0);
    ue(w, 0);                                   /* log2_parallel_merge_level_minus2 */
    u1(w, 0); u1(w, 0);
    trailing(w);
}

size_t plh_hdr_sequence(const plh_hdr_cfg *c, uint8_t *buf, size_t cap)
{
    size_t o = 0, n;
    bw w;
    if (c->codec == PLH_CODEC_H264) {
        void (*f[2])(const plh_hdr_cfg *, bw *) = { h264_sps, h264_pps };
        for (int i = 0; i < 2; i++) {
            memset(&w, 0, sizeof w); f[i](c, &w);
            if (!(n = emit(&w, buf + o, cap - o, NULL))) return 0;
            o += n;
        }
    } else {
        void (*f[3])(const plh_hdr_cfg *, bw *) = { hevc_vps, hevc_sps, hevc_pps };
        for (int i = 0; i < 3; i++) {
            memset(&w, 0, sizeof w); f[i](c, &w);
            if (!(n = emit(&w, buf + o, cap - o, NULL))) return 0;
            o += n;
        }
    }
    return o;
}

size_t plh_hdr_slice(const plh_hdr_cfg *c, int idr, uint32_t frame_num, int32_t poc,
                     uint32_t idr_pic_id, int qp_delta, uint8_t *buf, size_t cap, uint32_t *bits)
{
    bw w;
    memset(&w, 0, sizeof w);
    if (c->codec == PLH_CODEC_H264) {
        h264_nal(&w, idr ? 3 : 2, idr ? 5 : 1);
        ue(&w, 0);                                  /* first_mb_in_slice */
        ue(&w, idr ? 7 : 5);                        /* I / P (all slices) */
        ue(&w, 0);                                  /* pps_id */
        put(&w, frame_num & ((1u << PLH_H264_LOG2_MAX_FRAME_NUM) - 1), PLH_H264_LOG2_MAX_FRAME_NUM);
        if (idr) ue(&w, idr_pic_id & 0xffff);
        put(&w, (uint32_t)poc & ((1u << PLH_H264_LOG2_MAX_POC_LSB) - 1), PLH_H264_LOG2_MAX_POC_LSB);
        if (!idr) { u1(&w, 0); u1(&w, 0); }         /* override, ref_pic_list_modification_l0 */
        if (idr) { u1(&w, 0); u1(&w, 0); }          /* no_output_of_prior_pics, long_term_reference */
        else u1(&w, 0);                             /* adaptive_ref_pic_marking */
        if (c->cabac && !idr) ue(&w, 0);            /* cabac_init_idc */
        se(&w, qp_delta);
        ue(&w, 0); se(&w, 0); se(&w, 0);            /* deblocking */
    } else {
        hevc_nal(&w, idr ? 19 : 1);
        u1(&w, 1);                                  /* first_slice_segment_in_pic */
        if (idr) u1(&w, 0);                         /* no_output_of_prior_pics */
        ue(&w, 0);                                  /* pps_id */
        ue(&w, idr ? 2 : 1);                        /* I / P */
        if (!idr) {
            put(&w, (uint32_t)poc & ((1u << PLH_HEVC_LOG2_MAX_POC_LSB) - 1), PLH_HEVC_LOG2_MAX_POC_LSB);
            u1(&w, 1);                              /* short_term_ref_pic_set_sps_flag */
        }
        if (c->sao) { u1(&w, 1); u1(&w, 1); }
        if (!idr) u1(&w, 0);                        /* num_ref_idx_active_override */
        if (!idr) ue(&w, 0);                        /* five_minus_max_num_merge_cand */
        se(&w, qp_delta);
        u1(&w, 1);                                  /* slice_loop_filter_across_slices */
        trailing(&w);                               /* byte_alignment() */
    }
    return emit(&w, buf, cap, bits);
}
