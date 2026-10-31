/*
 * amfrt64.dll (PE, mingw-w64) — implementation of the PUBLIC AMF API (SDK v1.5.3, MIT, headers
 * vendored in third_party/amf) on top of plh-bridge's encode service 0x02 (N3).
 * Owner: encodeur.  Unknown components/properties return a clean AMF error and are
 * logged (PLH_AMF_LOG, default Z:\tmp\plh-encodeur\amf.log).
 *
 * Pixel path: D3D11 texture -> staging copy -> shm slot -> ENC_FRAME -> packet -> AMFBuffer.
 */
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/amf/core/Factory.h"
#include "../third_party/amf/core/Context.h"
#include "../third_party/amf/core/Surface.h"
#include "../third_party/amf/core/Buffer.h"
#include "../third_party/amf/core/Plane.h"
#include "../third_party/amf/core/Version.h"
#include "../third_party/amf/components/Component.h"
#include "../third_party/amf/components/VideoEncoderVCE.h"
#include "../third_party/amf/components/VideoEncoderHEVC.h"

#include "../../bridge/client/plh_client.h"
#include "../proto/plh_proto_encode.h"

using namespace amf;

/* ------------------------------------------------------------------ journal */
static std::mutex g_logmu;
static void alog(const char *fmt, ...)
{
    std::lock_guard<std::mutex> l(g_logmu);
    static FILE *f; static bool tried;
    if (!tried) {
        tried = true;
        const char *p = getenv("PLH_AMF_LOG");
        if (!p) { CreateDirectoryA("Z:\\tmp\\plh-encodeur", NULL); p = "Z:\\tmp\\plh-encodeur\\amf.log"; }
        f = fopen(p, "a");
    }
    if (!f) return;
    fprintf(f, "[%lu %lu] ", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}
static std::string W8(const wchar_t *w)
{
    std::string s; if (!w) return "(null)";
    for (; *w; w++) s += (*w < 128) ? (char)*w : '?';
    return s;
}
static std::string vstr(const AMFVariantStruct *v)
{
    char b[96];
    switch (v->type) {
    case AMF_VARIANT_EMPTY: return "empty";
    case AMF_VARIANT_BOOL: return v->boolValue ? "true" : "false";
    case AMF_VARIANT_INT64: snprintf(b, sizeof b, "int64 %lld", (long long)v->int64Value); return b;
    case AMF_VARIANT_DOUBLE: snprintf(b, sizeof b, "double %g", v->doubleValue); return b;
    case AMF_VARIANT_SIZE: snprintf(b, sizeof b, "size %dx%d", v->sizeValue.width, v->sizeValue.height); return b;
    case AMF_VARIANT_RATE: snprintf(b, sizeof b, "rate %u/%u", v->rateValue.num, v->rateValue.den); return b;
    case AMF_VARIANT_RATIO: snprintf(b, sizeof b, "ratio %u/%u", v->ratioValue.num, v->ratioValue.den); return b;
    case AMF_VARIANT_WSTRING: return "wstr " + W8(v->wstringValue);
    case AMF_VARIANT_INTERFACE: return "interface";
    default: snprintf(b, sizeof b, "type %d", (int)v->type); return b;
    }
}
static bool same_iid(const AMFGuid &a, const AMFGuid &b) { return memcmp(&a, &b, sizeof a) == 0; }

/* ---------------------------------------------------------- property storage */
struct PVal { AMFVariantStruct v; std::wstring ws; };
struct Props {
    std::map<std::wstring, PVal> m;
    std::mutex mu;
    AMF_RESULT set(const wchar_t *n, const AMFVariantStruct &val)
    {
        if (!n) return AMF_INVALID_POINTER;
        std::lock_guard<std::mutex> l(mu);
        PVal &p = m[n];
        p.v = val;
        if (val.type == AMF_VARIANT_WSTRING) { p.ws = val.wstringValue ? val.wstringValue : L""; p.v.wstringValue = (wchar_t *)p.ws.c_str(); }
        else if (val.type == AMF_VARIANT_STRING) { p.ws.clear(); p.v.type = AMF_VARIANT_EMPTY; }
        if (val.type == AMF_VARIANT_INTERFACE && val.pInterface) val.pInterface->Acquire();
        return AMF_OK;
    }
    AMF_RESULT get(const wchar_t *n, AMFVariantStruct *out)
    {
        if (!n || !out) return AMF_INVALID_POINTER;
        std::lock_guard<std::mutex> l(mu);
        auto it = m.find(n);
        if (it == m.end()) return AMF_NOT_FOUND;
        *out = it->second.v;
        return AMF_OK;
    }
    bool geti(const wchar_t *n, int64_t *o)
    {
        AMFVariantStruct v; if (get(n, &v) != AMF_OK) return false;
        if (v.type == AMF_VARIANT_INT64) *o = v.int64Value; else if (v.type == AMF_VARIANT_BOOL) *o = v.boolValue;
        else return false;
        return true;
    }
};

#define AMF_REFCOUNT() \
    std::atomic<long> rc_{1}; \
    amf_long AMF_STD_CALL Acquire() override { return ++rc_; } \
    amf_long AMF_STD_CALL Release() override { long r = --rc_; if (r == 0) delete this; return r; }

#define AMF_PS_IMPL() \
    Props props_; \
    AMF_RESULT AMF_STD_CALL SetProperty(const wchar_t *n, AMFVariantStruct v) override { return OnSet(n, v); } \
    AMF_RESULT AMF_STD_CALL GetProperty(const wchar_t *n, AMFVariantStruct *v) const override { return const_cast<Props &>(props_).get(n, v); } \
    amf_bool AMF_STD_CALL HasProperty(const wchar_t *n) const override { AMFVariantStruct v; return const_cast<Props &>(props_).get(n, &v) == AMF_OK; } \
    amf_size AMF_STD_CALL GetPropertyCount() const override { return props_.m.size(); } \
    AMF_RESULT AMF_STD_CALL GetPropertyAt(amf_size i, wchar_t *name, amf_size ns, AMFVariantStruct *v) const override { \
        if (i >= props_.m.size()) return AMF_OUT_OF_RANGE; auto it = props_.m.begin(); std::advance(it, i); \
        if (name && ns) { wcsncpy(name, it->first.c_str(), ns - 1); name[ns - 1] = 0; } if (v) *v = it->second.v; return AMF_OK; } \
    AMF_RESULT AMF_STD_CALL Clear() override { props_.m.clear(); return AMF_OK; } \
    AMF_RESULT AMF_STD_CALL AddTo(AMFPropertyStorage *d, amf_bool ow, amf_bool) const override { \
        if (!d) return AMF_INVALID_POINTER; for (auto &kv : props_.m) { if (!ow && d->HasProperty(kv.first.c_str())) continue; d->SetProperty(kv.first.c_str(), kv.second.v); } return AMF_OK; } \
    AMF_RESULT AMF_STD_CALL CopyTo(AMFPropertyStorage *d, amf_bool deep) const override { if (!d) return AMF_INVALID_POINTER; d->Clear(); return AddTo(d, true, deep); } \
    void AMF_STD_CALL AddObserver(AMFPropertyStorageObserver *) override {} \
    void AMF_STD_CALL RemoveObserver(AMFPropertyStorageObserver *) override {}

#define AMF_PSX_IMPL() \
    AMF_PS_IMPL() \
    amf_size AMF_STD_CALL GetPropertiesInfoCount() const override { return 0; } \
    AMF_RESULT AMF_STD_CALL GetPropertyInfo(const wchar_t *, const AMFPropertyInfo **) const override { return AMF_NOT_FOUND; } \
    AMF_RESULT AMF_STD_CALL GetPropertyInfo(amf_size, const AMFPropertyInfo **) const override { return AMF_OUT_OF_RANGE; } \
    AMF_RESULT AMF_STD_CALL ValidateProperty(const wchar_t *, AMFVariantStruct v, AMFVariantStruct *o) const override { if (o) *o = v; return AMF_OK; }

/* ------------------------------------------------------------------- plane / surface / buffer */
struct PlaneImpl : AMFPlane {
    AMF_REFCOUNT()
    AMF_PLANE_TYPE type; void *ptr; int w, h, pitch, vpitch, px;
    PlaneImpl(AMF_PLANE_TYPE t, void *p, int w_, int h_, int pitch_, int px_) : type(t), ptr(p), w(w_), h(h_), pitch(pitch_), vpitch(h_), px(px_) {}
    AMF_RESULT AMF_STD_CALL QueryInterface(const AMFGuid &i, void **pp) override
    { if (!pp) return AMF_INVALID_POINTER; if (same_iid(i, AMFInterface::IID()) || same_iid(i, AMFPlane::IID())) { *pp = this; Acquire(); return AMF_OK; } *pp = NULL; return AMF_NO_INTERFACE; }
    AMF_PLANE_TYPE AMF_STD_CALL GetType() override { return type; }
    void *AMF_STD_CALL GetNative() override { return ptr; }
    amf_int32 AMF_STD_CALL GetPixelSizeInBytes() override { return px; }
    amf_int32 AMF_STD_CALL GetOffsetX() override { return 0; }
    amf_int32 AMF_STD_CALL GetOffsetY() override { return 0; }
    amf_int32 AMF_STD_CALL GetWidth() override { return w; }
    amf_int32 AMF_STD_CALL GetHeight() override { return h; }
    amf_int32 AMF_STD_CALL GetHPitch() override { return pitch; }
    amf_int32 AMF_STD_CALL GetVPitch() override { return vpitch; }
    bool AMF_STD_CALL IsTiled() override { return false; }
};

struct SurfaceImpl : AMFSurface {
    AMF_REFCOUNT()
    AMF_PS_IMPL()
    AMF_RESULT OnSet(const wchar_t *n, const AMFVariantStruct &v) { return props_.set(n, v); }
    AMF_MEMORY_TYPE mem; AMF_SURFACE_FORMAT fmt; int w, h;
    std::vector<uint8_t> host; int pitch = 0;           /* HOST */
    ID3D11Texture2D *tex = nullptr;                     /* DX11 */
    amf_pts pts = 0, dur = 0; AMF_FRAME_TYPE ftype = AMF_FRAME_UNKNOWN;
    std::vector<PlaneImpl *> planes;
    SurfaceImpl(AMF_MEMORY_TYPE m, AMF_SURFACE_FORMAT f, int w_, int h_) : mem(m), fmt(f), w(w_), h(h_) {}
    ~SurfaceImpl() { if (tex) tex->Release(); for (auto p : planes) p->Release(); }
    void build_planes()
    {
        if (mem != AMF_MEMORY_HOST) return;
        int bpp = (fmt == AMF_SURFACE_NV12) ? 1 : 4;
        pitch = w * bpp; host.assign((size_t)pitch * h * (fmt == AMF_SURFACE_NV12 ? 3 : 2) / (fmt == AMF_SURFACE_NV12 ? 2 : 1), 0);
        if (fmt == AMF_SURFACE_NV12) {
            planes.push_back(new PlaneImpl(AMF_PLANE_Y, host.data(), w, h, pitch, 1));
            planes.push_back(new PlaneImpl(AMF_PLANE_UV, host.data() + (size_t)pitch * h, w / 2, h / 2, pitch, 2));
        } else planes.push_back(new PlaneImpl(AMF_PLANE_PACKED, host.data(), w, h, pitch, 4));
    }
    AMF_RESULT AMF_STD_CALL QueryInterface(const AMFGuid &i, void **pp) override
    {
        if (!pp) return AMF_INVALID_POINTER;
        if (same_iid(i, AMFInterface::IID()) || same_iid(i, AMFPropertyStorage::IID()) || same_iid(i, AMFData::IID()) || same_iid(i, AMFSurface::IID())) { *pp = this; Acquire(); return AMF_OK; }
        *pp = NULL; return AMF_NO_INTERFACE;
    }
    AMF_MEMORY_TYPE AMF_STD_CALL GetMemoryType() override { return mem; }
    AMF_RESULT AMF_STD_CALL Duplicate(AMF_MEMORY_TYPE, AMFData **) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL Convert(AMF_MEMORY_TYPE t) override { return t == mem ? AMF_OK : AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL Interop(AMF_MEMORY_TYPE t) override { return t == mem ? AMF_OK : AMF_NOT_SUPPORTED; }
    AMF_DATA_TYPE AMF_STD_CALL GetDataType() override { return AMF_DATA_SURFACE; }
    amf_bool AMF_STD_CALL IsReusable() override { return true; }
    void AMF_STD_CALL SetPts(amf_pts p) override { pts = p; }
    amf_pts AMF_STD_CALL GetPts() override { return pts; }
    void AMF_STD_CALL SetDuration(amf_pts d) override { dur = d; }
    amf_pts AMF_STD_CALL GetDuration() override { return dur; }
    AMF_SURFACE_FORMAT AMF_STD_CALL GetFormat() override { return fmt; }
    amf_size AMF_STD_CALL GetPlanesCount() override { return planes.size(); }
    AMFPlane *AMF_STD_CALL GetPlaneAt(amf_size i) override { return i < planes.size() ? planes[i] : nullptr; }
    AMFPlane *AMF_STD_CALL GetPlane(AMF_PLANE_TYPE t) override { for (auto p : planes) if (p->type == t) return p; return nullptr; }
    AMF_FRAME_TYPE AMF_STD_CALL GetFrameType() override { return ftype; }
    void AMF_STD_CALL SetFrameType(AMF_FRAME_TYPE t) override { ftype = t; }
    AMF_RESULT AMF_STD_CALL SetCrop(amf_int32, amf_int32, amf_int32, amf_int32) override { return AMF_OK; }
    AMF_RESULT AMF_STD_CALL CopySurfaceRegion(AMFSurface *, amf_int32, amf_int32, amf_int32, amf_int32, amf_int32, amf_int32) override { return AMF_NOT_SUPPORTED; }
    void AMF_STD_CALL AddObserver(AMFSurfaceObserver *) override {}
    void AMF_STD_CALL RemoveObserver(AMFSurfaceObserver *) override {}
};

struct BufferImpl : AMFBuffer {
    AMF_REFCOUNT()
    AMF_PS_IMPL()
    AMF_RESULT OnSet(const wchar_t *n, const AMFVariantStruct &v) { return props_.set(n, v); }
    std::vector<uint8_t> data; amf_pts pts = 0, dur = 0;
    AMF_RESULT AMF_STD_CALL QueryInterface(const AMFGuid &i, void **pp) override
    {
        if (!pp) return AMF_INVALID_POINTER;
        if (same_iid(i, AMFInterface::IID()) || same_iid(i, AMFPropertyStorage::IID()) || same_iid(i, AMFData::IID()) || same_iid(i, AMFBuffer::IID())) { *pp = this; Acquire(); return AMF_OK; }
        *pp = NULL; return AMF_NO_INTERFACE;
    }
    AMF_MEMORY_TYPE AMF_STD_CALL GetMemoryType() override { return AMF_MEMORY_HOST; }
    AMF_RESULT AMF_STD_CALL Duplicate(AMF_MEMORY_TYPE t, AMFData **pp) override
    {
        if (t != AMF_MEMORY_HOST || !pp) return AMF_NOT_SUPPORTED;
        BufferImpl *b = new BufferImpl; b->data = data; b->pts = pts; b->dur = dur; *pp = b; return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL Convert(AMF_MEMORY_TYPE t) override { return t == AMF_MEMORY_HOST ? AMF_OK : AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL Interop(AMF_MEMORY_TYPE t) override { return t == AMF_MEMORY_HOST ? AMF_OK : AMF_NOT_SUPPORTED; }
    AMF_DATA_TYPE AMF_STD_CALL GetDataType() override { return AMF_DATA_BUFFER; }
    amf_bool AMF_STD_CALL IsReusable() override { return true; }
    void AMF_STD_CALL SetPts(amf_pts p) override { pts = p; }
    amf_pts AMF_STD_CALL GetPts() override { return pts; }
    void AMF_STD_CALL SetDuration(amf_pts d) override { dur = d; }
    amf_pts AMF_STD_CALL GetDuration() override { return dur; }
    AMF_RESULT AMF_STD_CALL SetSize(amf_size n) override { data.resize(n); return AMF_OK; }
    amf_size AMF_STD_CALL GetSize() override { return data.size(); }
    void *AMF_STD_CALL GetNative() override { return data.data(); }
    void AMF_STD_CALL AddObserver(AMFBufferObserver *) override {}
    void AMF_STD_CALL RemoveObserver(AMFBufferObserver *) override {}
};

/* ---------------------------------------------------------------- encoder component */
struct Context;

struct Encoder : AMFComponent {
    AMF_REFCOUNT()
    AMF_PSX_IMPL()
    Context *ctx; bool hevc; std::mutex mu;
    struct plh_conn *conn = nullptr; bool open = false, drained = false;
    uint32_t sid = 0; struct plh_enc_open_reply orp; struct plh_shm_map shm; bool shm_ok = false;
    int w = 0, h = 0; AMF_SURFACE_FORMAT fmt = AMF_SURFACE_UNKNOWN;
    std::deque<BufferImpl *> out;
    ID3D11Texture2D *stage = nullptr; D3D11_TEXTURE2D_DESC stage_desc{};
    uint64_t nframes = 0;

    Encoder(Context *c, bool hevc_);
    ~Encoder();
    AMF_RESULT OnSet(const wchar_t *n, const AMFVariantStruct &v)
    {
        alog("%s SetProperty %s = %s", hevc ? "HEVC" : "AVC", W8(n).c_str(), vstr(&v).c_str());
        AMF_RESULT r = props_.set(n, v);
        if (open) apply_live(n, v);
        return r;
    }
    void apply_live(const wchar_t *n, const AMFVariantStruct &v);
    /* read a property by "base" name (Hevc prefix removed) */
    bool geti(const wchar_t *base, int64_t *o)
    {
        std::wstring b = base;
        if (props_.geti(b.c_str(), o)) return true;
        return props_.geti((L"Hevc" + b).c_str(), o);
    }
    bool getv(const wchar_t *base, AMFVariantStruct *v)
    {
        std::wstring b = base;
        if (props_.get(b.c_str(), v) == AMF_OK) return true;
        return props_.get((L"Hevc" + b).c_str(), v) == AMF_OK;
    }

    AMF_RESULT AMF_STD_CALL QueryInterface(const AMFGuid &i, void **pp) override
    {
        if (!pp) return AMF_INVALID_POINTER;
        if (same_iid(i, AMFInterface::IID()) || same_iid(i, AMFPropertyStorage::IID()) || same_iid(i, AMFPropertyStorageEx::IID()) || same_iid(i, AMFComponent::IID())) { *pp = this; Acquire(); return AMF_OK; }
        alog("Encoder::QueryInterface unknown IID %08x", (unsigned)i.data1);
        *pp = NULL; return AMF_NO_INTERFACE;
    }
    AMF_RESULT AMF_STD_CALL Init(AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height) override;
    AMF_RESULT AMF_STD_CALL ReInit(amf_int32 width, amf_int32 height) override;
    AMF_RESULT AMF_STD_CALL Terminate() override;
    AMF_RESULT AMF_STD_CALL Drain() override { alog("Drain"); drained = true; return AMF_OK; }
    AMF_RESULT AMF_STD_CALL Flush() override { alog("Flush"); std::lock_guard<std::mutex> l(mu); for (auto b : out) b->Release(); out.clear(); return AMF_OK; }
    AMF_RESULT AMF_STD_CALL SubmitInput(AMFData *d) override;
    AMF_RESULT AMF_STD_CALL QueryOutput(AMFData **pp) override;
    AMFContext *AMF_STD_CALL GetContext() override;
    AMF_RESULT AMF_STD_CALL SetOutputDataAllocatorCB(AMFDataAllocatorCB *) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL GetCaps(AMFCaps **) override { alog("GetCaps -> NOT_SUPPORTED"); return AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL Optimize(AMFComponentOptimizationCallback *) override { return AMF_OK; }
    void close_session();
    AMF_RESULT read_texture(SurfaceImpl *s, uint8_t *slot, uint32_t stride, uint32_t *uvoff, uint32_t *pf, uint32_t *rw, uint32_t *rh);
};

/* ---------------------------------------------------------------------- contexte */
#define NS(...) { alog("Context::%s -> NOT_SUPPORTED", __func__); return AMF_NOT_SUPPORTED; }
struct Context : AMFContext3 {
    AMF_REFCOUNT()
    AMF_PS_IMPL()
    AMF_RESULT OnSet(const wchar_t *n, const AMFVariantStruct &v) { alog("Context SetProperty %s = %s", W8(n).c_str(), vstr(&v).c_str()); return props_.set(n, v); }
    ID3D11Device *dev = nullptr; std::mutex dxmu;

    ~Context() { if (dev) dev->Release(); }
    AMF_RESULT AMF_STD_CALL QueryInterface(const AMFGuid &i, void **pp) override
    {
        if (!pp) return AMF_INVALID_POINTER;
        if (same_iid(i, AMFInterface::IID()) || same_iid(i, AMFPropertyStorage::IID()) || same_iid(i, AMFContext::IID()) ||
            same_iid(i, AMFContext1::IID()) || same_iid(i, AMFContext2::IID()) || same_iid(i, AMFContext3::IID())) { *pp = this; Acquire(); return AMF_OK; }
        alog("Context::QueryInterface unknown IID %08x", (unsigned)i.data1);
        *pp = NULL; return AMF_NO_INTERFACE;
    }
    AMF_RESULT AMF_STD_CALL Terminate() override { if (dev) { dev->Release(); dev = nullptr; } return AMF_OK; }
    AMF_RESULT AMF_STD_CALL InitDX9(void *) override NS()
    void *AMF_STD_CALL GetDX9Device(AMF_DX_VERSION) override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockDX9() override NS()
    AMF_RESULT AMF_STD_CALL UnlockDX9() override NS()
    AMF_RESULT AMF_STD_CALL InitDX11(void *d, AMF_DX_VERSION v) override
    {
        alog("Context::InitDX11 device=%p version=%d", d, (int)v);
        /* bridge probe: without plh-bridge there is no encoder -> Parsec must fall back to something else */
        struct plh_conn *c = nullptr; struct plh_hello_reply hr;
        if (plh_conn_open(&c, PLH_CLIENT_ENCODER, "amfrt64-probe", &hr)) { alog("InitDX11: plh-bridge unreachable -> AMF_NO_DEVICE"); return AMF_NO_DEVICE; }
        plh_conn_close(c);
        if (d) {
            ID3D11Device *nd = (ID3D11Device *)d;
            std::lock_guard<std::mutex> l(dxmu);
            if (dev) dev->Release();
            dev = nd; dev->AddRef();
        }
        return AMF_OK;
    }
    void *AMF_STD_CALL GetDX11Device(AMF_DX_VERSION) override { return dev; }
    AMF_RESULT AMF_STD_CALL LockDX11() override { dxmu.lock(); return AMF_OK; }
    AMF_RESULT AMF_STD_CALL UnlockDX11() override { dxmu.unlock(); return AMF_OK; }
    AMF_RESULT AMF_STD_CALL InitOpenCL(void *) override NS()
    void *AMF_STD_CALL GetOpenCLContext() override { return nullptr; }
    void *AMF_STD_CALL GetOpenCLCommandQueue() override { return nullptr; }
    void *AMF_STD_CALL GetOpenCLDeviceID() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL GetOpenCLComputeFactory(AMFComputeFactory **) override NS()
    AMF_RESULT AMF_STD_CALL InitOpenCLEx(AMFComputeDevice *) override NS()
    AMF_RESULT AMF_STD_CALL LockOpenCL() override NS()
    AMF_RESULT AMF_STD_CALL UnlockOpenCL() override NS()
    AMF_RESULT AMF_STD_CALL InitOpenGL(amf_handle, amf_handle, amf_handle) override NS()
    amf_handle AMF_STD_CALL GetOpenGLContext() override { return nullptr; }
    amf_handle AMF_STD_CALL GetOpenGLDrawable() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockOpenGL() override NS()
    AMF_RESULT AMF_STD_CALL UnlockOpenGL() override NS()
    AMF_RESULT AMF_STD_CALL InitXV(void *) override NS()
    void *AMF_STD_CALL GetXVDevice() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockXV() override NS()
    AMF_RESULT AMF_STD_CALL UnlockXV() override NS()
    AMF_RESULT AMF_STD_CALL InitGralloc(void *) override NS()
    void *AMF_STD_CALL GetGrallocDevice() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockGralloc() override NS()
    AMF_RESULT AMF_STD_CALL UnlockGralloc() override NS()

    AMF_RESULT AMF_STD_CALL AllocBuffer(AMF_MEMORY_TYPE t, amf_size n, AMFBuffer **pp) override
    {
        alog("AllocBuffer type=%d size=%zu", (int)t, (size_t)n);
        if (!pp) return AMF_INVALID_POINTER; BufferImpl *b = new BufferImpl; b->data.resize(n); *pp = b; return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL AllocSurface(AMF_MEMORY_TYPE t, AMF_SURFACE_FORMAT f, amf_int32 w, amf_int32 h, AMFSurface **pp) override
    {
        alog("AllocSurface type=%d fmt=%d %dx%d", (int)t, (int)f, w, h);
        if (!pp) return AMF_INVALID_POINTER;
        if (f != AMF_SURFACE_NV12 && f != AMF_SURFACE_BGRA && f != AMF_SURFACE_RGBA) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
        SurfaceImpl *s = new SurfaceImpl(t, f, w, h);
        if (t == AMF_MEMORY_HOST) { s->build_planes(); }
        else if (t == AMF_MEMORY_DX11) {
            if (!dev) { delete s; return AMF_NOT_INITIALIZED; }
            D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
            d.Format = f == AMF_SURFACE_NV12 ? DXGI_FORMAT_NV12 : f == AMF_SURFACE_BGRA ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            HRESULT hr = dev->CreateTexture2D(&d, NULL, &s->tex);
            if (FAILED(hr)) { alog("AllocSurface DX11: CreateTexture2D 0x%lx", (unsigned long)hr); delete s; return AMF_DIRECTX_FAILED; }
        } else { delete s; return AMF_NOT_SUPPORTED; }
        *pp = s; return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL AllocAudioBuffer(AMF_MEMORY_TYPE, AMF_AUDIO_FORMAT, amf_int32, amf_int32, amf_int32, AMFAudioBuffer **) override NS()
    AMF_RESULT AMF_STD_CALL CreateBufferFromHostNative(void *p, amf_size n, AMFBuffer **pp, AMFBufferObserver *) override
    {
        if (!pp || !p) return AMF_INVALID_POINTER; BufferImpl *b = new BufferImpl; b->data.assign((uint8_t *)p, (uint8_t *)p + n); *pp = b; return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromHostNative(AMF_SURFACE_FORMAT f, amf_int32 w, amf_int32 h, amf_int32 hp, amf_int32 vp, void *data, AMFSurface **pp, AMFSurfaceObserver *) override
    {
        alog("CreateSurfaceFromHostNative fmt=%d %dx%d pitch=%d", (int)f, w, h, hp);
        if (!pp || !data) return AMF_INVALID_POINTER;
        if (f != AMF_SURFACE_NV12 && f != AMF_SURFACE_BGRA && f != AMF_SURFACE_RGBA) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
        SurfaceImpl *s = new SurfaceImpl(AMF_MEMORY_HOST, f, w, h);
        s->pitch = hp; size_t sz = (size_t)hp * (vp ? vp : h) * (f == AMF_SURFACE_NV12 ? 3 : 2) / (f == AMF_SURFACE_NV12 ? 2 : 1);
        s->host.assign((uint8_t *)data, (uint8_t *)data + sz);
        if (f == AMF_SURFACE_NV12) {
            s->planes.push_back(new PlaneImpl(AMF_PLANE_Y, s->host.data(), w, h, hp, 1));
            s->planes.push_back(new PlaneImpl(AMF_PLANE_UV, s->host.data() + (size_t)hp * (vp ? vp : h), w / 2, h / 2, hp, 2));
        } else s->planes.push_back(new PlaneImpl(AMF_PLANE_PACKED, s->host.data(), w, h, hp, 4));
        *pp = s; return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromDX9Native(void *, AMFSurface **, AMFSurfaceObserver *) override NS()
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromDX11Native(void *t, AMFSurface **pp, AMFSurfaceObserver *) override
    {
        if (!t || !pp) return AMF_INVALID_POINTER;
        ID3D11Texture2D *tx = (ID3D11Texture2D *)t; D3D11_TEXTURE2D_DESC d; tx->GetDesc(&d);
        AMF_SURFACE_FORMAT f = d.Format == DXGI_FORMAT_NV12 ? AMF_SURFACE_NV12 : d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? AMF_SURFACE_BGRA :
                               d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? AMF_SURFACE_RGBA : AMF_SURFACE_UNKNOWN;
        alog("CreateSurfaceFromDX11Native %ux%u dxgi=%d -> amf fmt %d", d.Width, d.Height, (int)d.Format, (int)f);
        if (f == AMF_SURFACE_UNKNOWN) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
        SurfaceImpl *s = new SurfaceImpl(AMF_MEMORY_DX11, f, d.Width, d.Height); s->tex = tx; tx->AddRef(); *pp = s; return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromOpenGLNative(AMF_SURFACE_FORMAT, amf_handle, AMFSurface **, AMFSurfaceObserver *) override NS()
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromGrallocNative(amf_handle, AMFSurface **, AMFSurfaceObserver *) override NS()
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromOpenCLNative(AMF_SURFACE_FORMAT, amf_int32, amf_int32, void **, AMFSurface **, AMFSurfaceObserver *) override NS()
    AMF_RESULT AMF_STD_CALL CreateBufferFromOpenCLNative(void *, amf_size, AMFBuffer **) override NS()
    AMF_RESULT AMF_STD_CALL GetCompute(AMF_MEMORY_TYPE, AMFCompute **) override NS()
    /* AMFContext1 */
    AMF_RESULT AMF_STD_CALL CreateBufferFromDX11Native(void *, AMFBuffer **, AMFBufferObserver *) override NS()
    AMF_RESULT AMF_STD_CALL AllocBufferEx(AMF_MEMORY_TYPE t, amf_size n, AMF_BUFFER_USAGE, AMF_MEMORY_CPU_ACCESS, AMFBuffer **pp) override { return AllocBuffer(t, n, pp); }
    AMF_RESULT AMF_STD_CALL AllocSurfaceEx(AMF_MEMORY_TYPE t, AMF_SURFACE_FORMAT f, amf_int32 w, amf_int32 h, AMF_SURFACE_USAGE, AMF_MEMORY_CPU_ACCESS, AMFSurface **pp) override { return AllocSurface(t, f, w, h, pp); }
    AMF_RESULT AMF_STD_CALL InitVulkan(void *) override NS()
    void *AMF_STD_CALL GetVulkanDevice() override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockVulkan() override NS()
    AMF_RESULT AMF_STD_CALL UnlockVulkan() override NS()
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromVulkanNative(void *, AMFSurface **, AMFSurfaceObserver *) override NS()
    AMF_RESULT AMF_STD_CALL CreateBufferFromVulkanNative(void *, AMFBuffer **, AMFBufferObserver *) override NS()
    AMF_RESULT AMF_STD_CALL GetVulkanDeviceExtensions(amf_size *, const char **) override NS()
    /* AMFContext2 */
    AMF_RESULT AMF_STD_CALL InitDX12(void *, AMF_DX_VERSION) override NS()
    void *AMF_STD_CALL GetDX12Device(AMF_DX_VERSION) override { return nullptr; }
    AMF_RESULT AMF_STD_CALL LockDX12() override NS()
    AMF_RESULT AMF_STD_CALL UnlockDX12() override NS()
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromDX12Native(void *, AMFSurface **, AMFSurfaceObserver *) override NS()
    AMF_RESULT AMF_STD_CALL CreateBufferFromDX12Native(void *, AMFBuffer **, AMFBufferObserver *) override NS()
    /* AMFContext3 */
    AMF_RESULT AMF_STD_CALL CreateSharedResourceFromSurface(AMFSurface *, AMFSharedSurfaceResource *) override NS()
    AMF_RESULT AMF_STD_CALL CreateSurfaceFromSharedResource(AMF_MEMORY_TYPE, const AMFSharedSurfaceResource *, AMFSurface **) override NS()
};

/* ------------------------------------------------------------- Encoder: implementation */
Encoder::Encoder(Context *c, bool hevc_) : ctx(c), hevc(hevc_) { memset(&orp, 0, sizeof orp); memset(&shm, 0, sizeof shm); ctx->Acquire(); }
Encoder::~Encoder() { close_session(); for (auto b : out) b->Release(); if (stage) stage->Release(); ctx->Release(); }
AMFContext *Encoder::GetContext() { return ctx; }

void Encoder::close_session()
{
    if (conn) {
        if (open) { struct plh_enc_session_ref ref = {}; ref.session_id = sid; uint32_t l; plh_conn_request(conn, PLH_MSG_ENC_CLOSE, &ref, sizeof ref, NULL, 0, &l, 3000); }
        if (shm_ok) plh_shm_unmap(&shm);
        plh_conn_close(conn);
    }
    conn = nullptr; open = false; shm_ok = false;
}

static uint32_t rcmap(int64_t m, bool hevc)
{
    (void)hevc;
    switch (m) { case 0: return PLH_ENC_RC_CQP; case 1: return PLH_ENC_RC_CBR; default: return PLH_ENC_RC_VBR; }
}

void Encoder::apply_live(const wchar_t *n, const AMFVariantStruct &v)
{
    std::wstring s = n; if (s.rfind(L"Hevc", 0) == 0) s = s.substr(4);
    if ((s == L"TargetBitrate" || s == L"PeakBitrate") && v.type == AMF_VARIANT_INT64 && conn) {
        int64_t t = 0, p = 0; geti(L"TargetBitrate", &t); geti(L"PeakBitrate", &p);
        struct plh_enc_reconfig rc = {}; rc.session_id = sid; rc.bitrate = (uint32_t)t; rc.peak_bitrate = (uint32_t)p;
        uint32_t l; int r = plh_conn_request(conn, PLH_MSG_ENC_RECONFIG, &rc, sizeof rc, NULL, 0, &l, 3000);
        alog("bitrate reconfig %lld/%lld -> %d", (long long)t, (long long)p, r);
    }
}

AMF_RESULT Encoder::Init(AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height)
{
    std::lock_guard<std::mutex> l(mu);
    alog("%s Init fmt=%d %dx%d", hevc ? "HEVC" : "AVC", (int)format, width, height);
    if (open) return AMF_ALREADY_INITIALIZED;
    uint32_t pf = format == AMF_SURFACE_NV12 ? PLH_FMT_NV12 : format == AMF_SURFACE_BGRA ? PLH_FMT_BGRA8 : format == AMF_SURFACE_RGBA ? PLH_FMT_RGBA8 : 0;
    if (!pf) return AMF_SURFACE_FORMAT_NOT_SUPPORTED;
    struct plh_hello_reply hr;
    if (plh_conn_open(&conn, PLH_CLIENT_ENCODER, "amfrt64", &hr)) { conn = nullptr; alog("Init: plh-bridge unreachable"); return AMF_NO_DEVICE; }

    struct plh_enc_open o; memset(&o, 0, sizeof o);
    int64_t v = 0; AMFVariantStruct vs;
    o.codec = hevc ? PLH_ENC_CODEC_HEVC : PLH_ENC_CODEC_H264;
    o.width = width; o.height = height; o.fps_num = 60; o.fps_den = 1;
    if (getv(L"FrameRate", &vs) && vs.type == AMF_VARIANT_RATE && vs.rateValue.num && vs.rateValue.den) { o.fps_num = vs.rateValue.num; o.fps_den = vs.rateValue.den; }
    if (getv(L"FrameSize", &vs) && vs.type == AMF_VARIANT_SIZE && vs.sizeValue.width > 0) { o.width = vs.sizeValue.width; o.height = vs.sizeValue.height; }
    o.rc = geti(L"RateControlMethod", &v) ? rcmap(v, hevc) : PLH_ENC_RC_CBR;
    o.bitrate = geti(L"TargetBitrate", &v) ? (uint32_t)v : 10000000;
    o.peak_bitrate = geti(L"PeakBitrate", &v) ? (uint32_t)v : o.bitrate;
    o.vbv_size = geti(L"VBVBufferSize", &v) ? (uint32_t)v : 0;
    if (geti(L"QPI", &v)) o.qp_i = (uint32_t)v;
    if (geti(L"QPP", &v)) o.qp_p = (uint32_t)v;
    if (geti(L"MinQP", &v)) o.min_qp = (uint32_t)v;
    if (geti(L"MaxQP", &v)) o.max_qp = (uint32_t)v;
    o.idr_period = 0;
    if (!hevc && geti(L"IDRPeriod", &v)) o.idr_period = (uint32_t)v;
    if (hevc && geti(L"GOPSize", &v)) { int64_t g = 1; geti(L"GOPSPerIDR", &g); o.idr_period = (uint32_t)(v * (g ? g : 1)); }
    o.quality = -1;
    if (geti(L"QualityPreset", &v)) o.quality = v == 0 ? 2 : v == 1 ? 1 : 0; /* AMF: 0 BALANCED?,... */
    if (hevc ? geti(L"NominalRange", &v) : geti(L"FullRangeColor", &v)) o.full_range = hevc ? (v == 0) : (v != 0);
    o.in_format = pf; o.in_slot_count = 2;
    alog("ENC_OPEN codec=%u %ux%u %u/%u rc=%u br=%u peak=%u idr=%u fmt=%u", o.codec, o.width, o.height, o.fps_num, o.fps_den, o.rc, o.bitrate, o.peak_bitrate, o.idr_period, o.in_format);

    uint32_t len = 0;
    int st = plh_conn_request(conn, PLH_MSG_ENC_OPEN, &o, sizeof o, &orp, sizeof orp, &len, 8000);
    if (st || orp.status != 0 || len < sizeof orp) { alog("ENC_OPEN failed st=%d status=%d", st, orp.status); close_session(); return AMF_CODEC_NOT_SUPPORTED; }
    if (plh_shm_map(&shm, orp.shm_name, orp.shm_size)) { alog("shm_map failed %s", orp.shm_name); sid = orp.session_id; open = true; close_session(); return AMF_FAIL; }
    shm_ok = true; sid = orp.session_id; open = true; w = o.width; h = o.height; fmt = format; drained = false;
    alog("session %u opened (backend %s)", sid, orp.backend);
    return AMF_OK;
}

AMF_RESULT Encoder::ReInit(amf_int32 width, amf_int32 height)
{
    alog("ReInit %dx%d", width, height);
    AMF_SURFACE_FORMAT f = fmt; { close_session(); }
    return Init(f, width, height);
}
AMF_RESULT Encoder::Terminate() { alog("Terminate"); std::lock_guard<std::mutex> l(mu); close_session(); return AMF_OK; }

AMF_RESULT Encoder::read_texture(SurfaceImpl *s, uint8_t *slot, uint32_t stride, uint32_t *uvoff, uint32_t *pf, uint32_t *rw, uint32_t *rh)
{
    ID3D11Texture2D *src = s->tex; D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    ID3D11Device *dev = nullptr; src->GetDevice(&dev); if (!dev) return AMF_DIRECTX_FAILED;
    ID3D11DeviceContext *dc = nullptr; dev->GetImmediateContext(&dc);
    if (!stage || stage_desc.Width != d.Width || stage_desc.Height != d.Height || stage_desc.Format != d.Format) {
        if (stage) { stage->Release(); stage = nullptr; }
        D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
        HRESULT hr = dev->CreateTexture2D(&sd, NULL, &stage);
        if (FAILED(hr)) { alog("staging CreateTexture2D 0x%lx", (unsigned long)hr); dc->Release(); dev->Release(); return AMF_DIRECTX_FAILED; }
        stage_desc = sd;
    }
    dc->CopySubresourceRegion(stage, 0, 0, 0, 0, src, 0, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = dc->Map(stage, 0, D3D11_MAP_READ, 0, &m);
    AMF_RESULT r = AMF_OK;
    if (FAILED(hr)) { alog("Map 0x%lx", (unsigned long)hr); r = AMF_DIRECTX_FAILED; }
    else {
        uint32_t cw = d.Width < (uint32_t)w ? d.Width : (uint32_t)w, ch = d.Height < (uint32_t)h ? d.Height : (uint32_t)h;
        *rw = cw; *rh = ch;
        const uint8_t *p = (const uint8_t *)m.pData;
        if (d.Format == DXGI_FORMAT_NV12) {
            for (uint32_t y = 0; y < ch; y++) memcpy(slot + (size_t)y * stride, p + (size_t)y * m.RowPitch, cw);
            *uvoff = stride * (uint32_t)h;
            const uint8_t *uv = p + (size_t)m.RowPitch * d.Height;
            for (uint32_t y = 0; y < (ch + 1) / 2; y++) memcpy(slot + *uvoff + (size_t)y * stride, uv + (size_t)y * m.RowPitch, cw);
            *pf = PLH_FMT_NV12;
        } else {
            for (uint32_t y = 0; y < ch; y++) memcpy(slot + (size_t)y * stride, p + (size_t)y * m.RowPitch, (size_t)cw * 4);
            *uvoff = 0; *pf = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? PLH_FMT_BGRA8 : PLH_FMT_RGBA8;
        }
        dc->Unmap(stage, 0);
    }
    dc->Release(); dev->Release();
    return r;
}

AMF_RESULT Encoder::SubmitInput(AMFData *data)
{
    if (!data) { alog("SubmitInput(NULL) = end of stream"); drained = true; return AMF_OK; }
    std::lock_guard<std::mutex> l(mu);
    if (!open) return AMF_NOT_INITIALIZED;
    AMFSurface *sf = nullptr;
    if (data->QueryInterface(AMFSurface::IID(), (void **)&sf) != AMF_OK) { alog("SubmitInput: not a surface"); return AMF_INVALID_DATA_TYPE; }
    SurfaceImpl *s = static_cast<SurfaceImpl *>(sf);   /* nos surfaces uniquement */
    uint8_t *shmb = (uint8_t *)shm.ptr; uint8_t *slot = shmb + orp.in_offset + (nframes % orp.in_slot_count) * orp.in_slot_size;
    uint32_t slotno = (uint32_t)(nframes % orp.in_slot_count);
    struct plh_enc_frame fr; memset(&fr, 0, sizeof fr);
    fr.session_id = sid; fr.source = PLH_ENC_SRC_SLOT; fr.slot = slotno; fr.stride = orp.in_stride; fr.pts = (uint64_t)s->GetPts();
    AMF_RESULT r = AMF_OK;
    if (s->mem == AMF_MEMORY_DX11 && s->tex) {
        uint32_t uvo = 0;
        r = read_texture(s, slot, orp.in_stride, &uvo, &fr.format, &fr.width, &fr.height);
        fr.uv_offset = uvo;
    } else if (s->mem == AMF_MEMORY_HOST && !s->planes.empty()) {
        uint32_t cw = (uint32_t)(s->w < w ? s->w : w), ch = (uint32_t)(s->h < h ? s->h : h);
        fr.width = cw; fr.height = ch;
        if (s->fmt == AMF_SURFACE_NV12) {
            fr.format = PLH_FMT_NV12; fr.uv_offset = (uint64_t)orp.in_stride * h;
            for (uint32_t y = 0; y < ch; y++) memcpy(slot + (size_t)y * orp.in_stride, (uint8_t *)s->planes[0]->ptr + (size_t)y * s->planes[0]->pitch, cw);
            for (uint32_t y = 0; y < (ch + 1) / 2; y++) memcpy(slot + fr.uv_offset + (size_t)y * orp.in_stride, (uint8_t *)s->planes[1]->ptr + (size_t)y * s->planes[1]->pitch, cw);
        } else {
            fr.format = s->fmt == AMF_SURFACE_BGRA ? PLH_FMT_BGRA8 : PLH_FMT_RGBA8;
            for (uint32_t y = 0; y < ch; y++) memcpy(slot + (size_t)y * orp.in_stride, (uint8_t *)s->planes[0]->ptr + (size_t)y * s->planes[0]->pitch, (size_t)cw * 4);
        }
    } else r = AMF_NOT_SUPPORTED;
    if (r != AMF_OK) { alog("SubmitInput: surface read failed %d (mem=%d)", (int)r, (int)s->mem); sf->Release(); return r; }

    /* forced IDR: property of the surface or of the component */
    int64_t ft = 0; AMFVariantStruct vv; bool force = false;
    const wchar_t *fk = hevc ? L"HevcForcePictureType" : L"ForcePictureType";
    if (s->props_.geti(fk, &ft) && ft == AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR) force = true;
    if (props_.geti(fk, &ft)) { if (ft == AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR) force = true; props_.set(fk, [] { AMFVariantStruct z; memset(&z, 0, sizeof z); z.type = AMF_VARIANT_INT64; return z; }()); }
    (void)vv;
    if (force) fr.flags |= PLH_ENC_FRAME_F_FORCE_IDR;
    if (nframes == 0) fr.flags |= PLH_ENC_FRAME_F_FORCE_IDR;

    struct plh_enc_frame_reply rr; uint32_t len = 0;
    int st = plh_conn_request(conn, PLH_MSG_ENC_FRAME, &fr, sizeof fr, &rr, sizeof rr, &len, 5000);
    sf->Release();
    if (st || rr.status != 0 || len < sizeof rr) { alog("ENC_FRAME failed st=%d status=%d", st, rr.status); return AMF_FAIL; }
    BufferImpl *b = new BufferImpl;
    b->data.assign(shmb + orp.out_offset, shmb + orp.out_offset + rr.size);
    b->pts = (amf_pts)rr.pts;
    AMFVariantStruct t; memset(&t, 0, sizeof t); t.type = AMF_VARIANT_INT64;
    t.int64Value = rr.keyframe ? 0 : 2; /* *_OUTPUT_DATA_TYPE_IDR = 0 ; P = 2 (H.264) ; HEVC : IDR 0, I 1, P 2 */
    b->props_.set(hevc ? L"HevcOutputDataType" : L"OutputDataType", t);
    out.push_back(b);
    nframes++;
    if (nframes <= 3 || nframes % 600 == 0) alog("frame %llu: %llu bytes key=%u encode=%lluus", (unsigned long long)nframes, (unsigned long long)rr.size, rr.keyframe, (unsigned long long)(rr.encode_ns / 1000));
    return AMF_OK;
}

AMF_RESULT Encoder::QueryOutput(AMFData **pp)
{
    if (!pp) return AMF_INVALID_POINTER;
    std::lock_guard<std::mutex> l(mu);
    if (out.empty()) { *pp = nullptr; return drained ? AMF_EOF : AMF_REPEAT; }
    BufferImpl *b = out.front(); out.pop_front();
    *pp = b; return AMF_OK;
}

/* ------------------------------------------------------------------------ fabrique */
struct Factory : AMFFactory {
    const wchar_t *cache = L"";
    AMF_RESULT AMF_STD_CALL CreateContext(AMFContext **pp) override { alog("CreateContext"); if (!pp) return AMF_INVALID_POINTER; *pp = new Context; return AMF_OK; }
    AMF_RESULT AMF_STD_CALL CreateComponent(AMFContext *c, const wchar_t *id, AMFComponent **pp) override
    {
        alog("CreateComponent %s", W8(id).c_str());
        if (!c || !id || !pp) return AMF_INVALID_POINTER;
        bool hevc;
        if (!wcscmp(id, AMFVideoEncoderVCE_AVC)) hevc = false;
        else if (!wcscmp(id, AMFVideoEncoder_HEVC)) hevc = true;
        else { alog("component not implemented : %s", W8(id).c_str()); *pp = nullptr; return AMF_NOT_FOUND; }
        *pp = new Encoder(static_cast<Context *>(c), hevc); return AMF_OK;
    }
    AMF_RESULT AMF_STD_CALL SetCacheFolder(const wchar_t *) override { return AMF_OK; }
    const wchar_t *AMF_STD_CALL GetCacheFolder() override { return cache; }
    AMF_RESULT AMF_STD_CALL GetDebug(AMFDebug **) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL GetTrace(AMFTrace **) override { return AMF_NOT_SUPPORTED; }
    AMF_RESULT AMF_STD_CALL GetPrograms(AMFPrograms **) override { return AMF_NOT_SUPPORTED; }
};
static Factory g_factory;

extern "C" {
__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL AMFQueryVersion(amf_uint64 *v)
{
    alog("AMFQueryVersion");
    if (!v) return AMF_INVALID_POINTER;
    *v = AMF_FULL_VERSION; return AMF_OK;
}
__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL AMFInit(amf_uint64 version, AMFFactory **pp)
{
    alog("AMFInit version=%llx", (unsigned long long)version);
    if (!pp) return AMF_INVALID_POINTER;
    *pp = &g_factory; return AMF_OK;
}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) alog("amfrt64.dll loaded (plh-encodeur N3)");
    return TRUE;
}
