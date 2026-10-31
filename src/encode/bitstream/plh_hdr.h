/* Generation of H.264 headers (SPS/PPS/slice header) and HEVC
 * (VPS/SPS/PPS/slice segment header) for a low-latency stream:
 * P only, 1 reference (previous frame), one slice per picture.
 * Annex B output (start codes + emulation-prevention bytes). */
#ifndef PLH_HDR_H
#define PLH_HDR_H

#include <stddef.h>
#include <stdint.h>
#include "plh_enc.h"

typedef struct plh_hdr_cfg {
    plh_codec codec;
    int       profile;           /* PLH_PROFILE_* */
    int       level_idc;         /* H.264 : 42 = 4.2 ; HEVC : 30 x niveau */
    uint32_t  width, height;     /* visible */
    uint32_t  coded_w, coded_h;  /* H.264: multiple of 16; HEVC: multiple of min CB */
    uint32_t  fps_num, fps_den;
    int       full_range;
    int       init_qp;
    /* H.264 */
    int       cabac, transform_8x8;
    /* HEVC */
    int       log2_min_cb_minus3, log2_diff_max_min_cb;
    int       log2_min_tb_minus2, log2_diff_max_min_tb;
    int       depth_inter, depth_intra;
    int       amp, sao, strong_intra, cu_qp_delta;
} plh_hdr_cfg;

#define PLH_H264_LOG2_MAX_FRAME_NUM 16
#define PLH_H264_LOG2_MAX_POC_LSB   16
#define PLH_HEVC_LOG2_MAX_POC_LSB   16

/* Sequence parameters (SPS+PPS or VPS+SPS+PPS). Returns the size in
 * bytes (0 if cap is insufficient). */
size_t plh_hdr_sequence(const plh_hdr_cfg *c, uint8_t *buf, size_t cap);

/* Slice header (NAL header included). *bits = exact length in bits
 * (H.264: not aligned; HEVC: aligned by byte_alignment()).
 * poc: full POC (H.264: 2*n; HEVC: n); frame_num: H.264 only. */
size_t plh_hdr_slice(const plh_hdr_cfg *c, int idr, uint32_t frame_num, int32_t poc,
                     uint32_t idr_pic_id, int slice_qp_delta,
                     uint8_t *buf, size_t cap, uint32_t *bits);

#endif
