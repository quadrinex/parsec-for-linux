/* Tests of the plh_enc API (N2): backend selection, errors, bitrate on the fly. TAP. */
#include "plh_enc.h"
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int n, fails;
#define CHECK(c, ...) do { n++; if (c) printf("ok %d - ", n); else { fails++; printf("not ok %d - ", n); } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void)
{
    printf("TAP version 13\n");
    plh_enc_caps caps;
    CHECK(plh_enc_query_caps(NULL, NULL, &caps) == 0 && !strcmp(caps.name, "vaapi"),
          "auto -> backend '%s' (%s)", caps.name, caps.device);
    CHECK(caps.h264 && caps.hevc && !caps.av1, "caps h264=%d (max %ux%u) hevc=%d (max %ux%u) av1=%d",
          caps.h264, caps.h264_max_w, caps.h264_max_h, caps.hevc, caps.hevc_max_w, caps.hevc_max_h, caps.av1);
    CHECK(plh_enc_query_caps("nonexistent", NULL, &caps) != 0, "unknown backend refused (caps)");

    plh_enc_params p = { .codec = PLH_CODEC_HEVC, .width = 1280, .height = 720, .rc = PLH_RC_CBR,
                         .bitrate = 8000000, .quality = -1 };
    char err[256];
    CHECK(plh_enc_create("nonexistent", NULL, &p, err, sizeof err) == NULL && strstr(err, "unknown"),
          "unknown backend refused (create): %s", err);
    CHECK(plh_enc_create("vaapi", "/dev/null/absent", &p, err, sizeof err) == NULL && err[0],
          "missing device: error '%s'", err);

    /* fallback: no such GPU -> VA-API and NVENC refuse, the software backend (x264) takes over */
    {
        plh_enc_params q = { .codec = PLH_CODEC_H264, .width = 640, .height = 360, .rc = PLH_RC_CBR,
                             .bitrate = 2000000, .quality = -1 };
        plh_encoder *f = plh_enc_create("auto", "/dev/null/absent", &q, err, sizeof err);
        CHECK(f && !strcmp(plh_enc_backend_name(f), "software"), "auto without GPU -> %s", f ? plh_enc_backend_name(f) : err);
        plh_enc_destroy(f);
        CHECK(plh_enc_query_caps("software", NULL, &caps) == 0 && caps.h264 && !caps.hevc, "caps software : %s", caps.device);
        CHECK(plh_enc_create("software", NULL, &p, err, sizeof err) == NULL && strstr(err, "HEVC"), "software refuse HEVC : %s", err);
        f = plh_enc_create("nvenc", NULL, &q, err, sizeof err);
        CHECK(!f && err[0], "nvenc without NVIDIA GPU: %s", err);
        CHECK(plh_enc_create("vulkan", NULL, &q, err, sizeof err) == NULL && strstr(err, "unknown"), "vulkan: backend absent (%s)", err);
    }

    plh_encoder *e = plh_enc_create(NULL, NULL, &p, err, sizeof err);
    CHECK(e && !strcmp(plh_enc_backend_name(e), "vaapi"), "auto creation -> %s", e ? plh_enc_backend_name(e) : err);
    if (!e) { printf("1..%d\n", n); return 1; }
    plh_surface *s = plh_enc_alloc_surface(e, DRM_FORMAT_XRGB8888);
    CHECK(s != NULL, "CPU BGRX surface");
    CHECK(plh_enc_alloc_surface(e, 0x12345678) == NULL && strstr(plh_enc_last_error(e), "not supported"),
          "unknown format refused: %s", plh_enc_last_error(e));
    uint8_t *px = calloc(1280 * 720, 4);
    const void *pl[2] = { px, NULL };
    const uint32_t st[2] = { 1280 * 4, 0 };
    plh_packet pk;
    int okall = 1, keys = 0;
    for (int i = 0; i < 10; i++) {
        memset(px, i * 20, 1280 * 720 * 4);
        if (i == 5) okall &= plh_enc_set_bitrate(e, 2000000, 0) == 0;
        okall &= plh_enc_upload(e, s, pl, st) == 0;
        okall &= plh_enc_encode(e, s, (uint64_t)i * 1000, i == 7 ? PLH_ENC_FORCE_IDR : 0, &pk) == 0;
        okall &= pk.pts == (uint64_t)i * 1000 && pk.frame_index == (uint64_t)i && pk.size > 0;
        keys |= pk.keyframe << i;
    }
    CHECK(okall, "10 frames encoded, pts/index preserved, set_bitrate accepted mid-stream (effect not measured here)");
    CHECK(keys == ((1 << 0) | (1 << 7)), "keyframe flags = frames 0 and 7 (mask 0x%x)", keys);
    CHECK(plh_enc_set_bitrate(e, 0, 0) != 0, "zero bitrate refused");
    plh_enc_release_surface(e, s);
    plh_enc_destroy(e);
    free(px);
    printf("1..%d\n", n);
    return fails != 0;
}
