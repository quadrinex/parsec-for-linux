/* Backend selection and dispatch functions. */
#define _POSIX_C_SOURCE 200809L
#include "plh_enc_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const struct plh_enc_backend *const backends[] = {
    &plh_enc_backend_vaapi,
    /* order = auto priority: VA-API (AMD/Intel) -> NVENC -> [Vulkan Video: absent] -> software */
    &plh_enc_backend_nvenc,
    &plh_enc_backend_software,
};
#define NBACKENDS (sizeof backends / sizeof *backends)

uint64_t plh_enc_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plh_enc_seterr(plh_encoder *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->err, sizeof e->err, fmt, ap);
    va_end(ap);
}

static const struct plh_enc_backend *find(const char *name)
{
    for (size_t i = 0; i < NBACKENDS; i++)
        if (!strcmp(backends[i]->name, name))
            return backends[i];
    return NULL;
}

int plh_enc_query_caps(const char *backend, const char *device, plh_enc_caps *caps)
{
    if (backend && strcmp(backend, "auto")) {
        const struct plh_enc_backend *b = find(backend);
        return b ? b->query_caps(device, caps) : -1;
    }
    for (size_t i = 0; i < NBACKENDS; i++)
        if (backends[i]->query_caps(device, caps) == 0)
            return 0;
    return -1;
}

plh_encoder *plh_enc_create(const char *backend, const char *device,
                            const plh_enc_params *p, char *err, size_t errlen)
{
    if (err && errlen)
        err[0] = 0;
    if (backend && strcmp(backend, "auto")) {
        const struct plh_enc_backend *b = find(backend);
        if (!b) {
            if (err) snprintf(err, errlen, "unknown backend: %s", backend);
            return NULL;
        }
        return b->create(device, p, err, errlen);
    }
    /* auto: first backend that accepts the parameters */
    for (size_t i = 0; i < NBACKENDS; i++) {
        plh_encoder *e = backends[i]->create(device, p, err, errlen);
        if (e)
            return e;
    }
    return NULL;
}

void plh_enc_destroy(plh_encoder *e) { if (e) e->be->destroy(e); }

plh_surface *plh_enc_import_dmabuf(plh_encoder *e, const plh_dmabuf *b)
{ return e->be->import_dmabuf ? e->be->import_dmabuf(e, b) : NULL; }

plh_surface *plh_enc_alloc_surface(plh_encoder *e, uint32_t f)
{ return e->be->alloc_surface(e, f); }

int plh_enc_upload(plh_encoder *e, plh_surface *s, const void *const pl[2],
                   const uint32_t st[2])
{ return e->be->upload(e, s, pl, st); }

void plh_enc_release_surface(plh_encoder *e, plh_surface *s)
{ if (s) e->be->release_surface(e, s); }

int plh_enc_encode(plh_encoder *e, plh_surface *in, uint64_t pts, unsigned flags,
                   plh_packet *out)
{ return e->be->encode(e, in, pts, flags, out); }

int plh_enc_set_bitrate(plh_encoder *e, uint32_t br, uint32_t peak)
{ return e->be->set_bitrate(e, br, peak); }

const char *plh_enc_backend_name(const plh_encoder *e) { return e->be->name; }
const char *plh_enc_last_error(const plh_encoder *e) { return e->err; }
