/* Internal interface of the encoder backends. Each backend puts
 * `struct plh_encoder` at the head of its private structure. */
#ifndef PLH_ENC_INTERNAL_H
#define PLH_ENC_INTERNAL_H

#include "plh_enc.h"

struct plh_enc_backend {
    const char *name;
    int  (*query_caps)(const char *device, plh_enc_caps *caps);
    plh_encoder *(*create)(const char *device, const plh_enc_params *p,
                           char *err, size_t errlen);
    void (*destroy)(plh_encoder *e);
    plh_surface *(*import_dmabuf)(plh_encoder *e, const plh_dmabuf *buf);
    plh_surface *(*alloc_surface)(plh_encoder *e, uint32_t drm_fourcc);
    int  (*upload)(plh_encoder *e, plh_surface *s, const void *const planes[2],
                   const uint32_t strides[2]);
    void (*release_surface)(plh_encoder *e, plh_surface *s);
    int  (*encode)(plh_encoder *e, plh_surface *in, uint64_t pts,
                   unsigned flags, plh_packet *out);
    int  (*set_bitrate)(plh_encoder *e, uint32_t bitrate, uint32_t peak);
};

struct plh_encoder {
    const struct plh_enc_backend *be;
    char err[256];
};

extern const struct plh_enc_backend plh_enc_backend_vaapi;
extern const struct plh_enc_backend plh_enc_backend_nvenc;
extern const struct plh_enc_backend plh_enc_backend_software;

void plh_enc_seterr(plh_encoder *e, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
uint64_t plh_enc_now_ns(void);

#endif
