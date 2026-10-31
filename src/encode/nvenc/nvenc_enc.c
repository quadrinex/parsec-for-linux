/* NVENC backend — PROBE only, UNTESTED (no NVIDIA GPU on this machine).
 * Real hardware detection: libcuda.so.1 (cuInit, cuDeviceGetCount) then
 * libnvidia-encode.so.1 (NvEncodeAPIGetMaxSupportedVersion). No hard dependency (dlopen).
 * The encode session (NV_ENC_*) is NOT implemented: the NVENC headers (nv-codec-headers)
 * are not installed and nothing here could verify it. create() therefore fails cleanly
 * and automatic selection moves on to the next backend. */
#define _POSIX_C_SOURCE 200809L
#include "../plh_enc_internal.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int nv_probe(char *desc, size_t n)
{
    void *cu = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!cu) return -1;
    int (*cuInit)(unsigned) = dlsym(cu, "cuInit");
    int (*cuDeviceGetCount)(int *) = dlsym(cu, "cuDeviceGetCount");
    int cnt = 0;
    if (!cuInit || !cuDeviceGetCount || cuInit(0) || cuDeviceGetCount(&cnt) || cnt < 1) { dlclose(cu); return -1; }
    void *ne = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!ne) { dlclose(cu); return -1; }
    int (*maxver)(uint32_t *) = dlsym(ne, "NvEncodeAPIGetMaxSupportedVersion");
    uint32_t v = 0;
    if (!maxver || maxver(&v)) { dlclose(ne); dlclose(cu); return -1; }
    snprintf(desc, n, "NVENC API %u.%u, %d GPU", v >> 4, v & 15, cnt);
    return 0;   /* libraries left loaded: the probe is infrequent */
}

static int nv_query_caps(const char *device, plh_enc_caps *c)
{
    (void)device;
    char d[128];
    if (nv_probe(d, sizeof d)) return -1;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "nvenc");
    snprintf(c->device, sizeof c->device, "%s", d);
    /* real capabilities not queried (session not implemented) */
    return 0;
}

static plh_encoder *nv_create(const char *device, const plh_enc_params *p, char *err, size_t errlen)
{
    (void)device; (void)p;
    char d[128];
    if (nv_probe(d, sizeof d)) snprintf(err, errlen, "nvenc: no NVIDIA GPU/libnvidia-encode");
    else snprintf(err, errlen, "nvenc: hardware present (%s) but encode session not implemented", d);
    return NULL;
}

const struct plh_enc_backend plh_enc_backend_nvenc = {
    .name = "nvenc", .query_caps = nv_query_caps, .create = nv_create,
};
