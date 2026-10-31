/* plh_trace.c — D3D11 tracer by patching DXVK vtables (external observation of public APIs). */
#include "plh_trace.h"
#include <dxgi1_5.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

static FILE *g_f; static CRITICAL_SECTION g_cs; static volatile LONG g_init, g_done;
static LONG g_cnt[64];
enum { H_TEX, H_TEX1, H_RTV, H_RTV1, H_SRV, H_SRV1, H_UAV, H_UAV1, H_OPENSH, H_COPYRES, H_COPYSUB, H_MAP, H_UNMAP,
       H_DRAW, H_DRAWIDX, H_DISPATCH, H_FLUSH, H_UPDSUB, H_CLEARRTV, H_QI, H_VPBLT, H_BUF, H_CLEARUAV, H_NK };
static const LONG g_budget[H_NK] = { 40, 40, 40, 40, 60, 60, 20, 20, 20, 40, 40, 30, 30, 30, 10, 30, 8, 8, 8, 80, 10, 20, 8 };
static const char *g_names[H_NK] = { "CreateTexture2D", "CreateTexture2D1", "CreateRenderTargetView", "CreateRenderTargetView1",
    "CreateShaderResourceView", "CreateShaderResourceView1", "CreateUnorderedAccessView", "CreateUnorderedAccessView1",
    "OpenSharedResource", "CopyResource", "CopySubresourceRegion", "Map", "Unmap", "Draw", "DrawIndexed", "Dispatch", "Flush",
    "UpdateSubresource", "ClearRenderTargetView", "Texture2D::QueryInterface", "VideoProcessorBlt", "CreateBuffer", "ClearUAV" };

int plh_trace_enabled(void) { const char *e = getenv("PLH_D3D_TRACE"); return e && *e == '1'; }
static BOOL CALLBACK once_open(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_cs);
    const char *path = getenv("PLH_D3D_TRACE_FILE");
    g_f = fopen(path ? path : "Z:\\tmp\\plh-d3d-trace.log", "w");
    return TRUE;
}
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
void plh_trace_msg(const char *fmt, ...)
{
    InitOnceExecuteOnce(&g_once, once_open, NULL, NULL);
    if (!g_f) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_f, "[%lu t%lu] ", (unsigned long)GetTickCount64() & 0xffffff, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_f, fmt, ap); va_end(ap);
    fputc('\n', g_f); fflush(g_f);
    LeaveCriticalSection(&g_cs);
}
/* true if this call must be logged (per-method budget); reports the end of the budget once */
static int bud(int h)
{
    LONG n = InterlockedIncrement(&g_cnt[h]);
    if (n == g_budget[h] + 1) plh_trace_msg("(%s: budget of %ld lines reached, later calls not logged)", g_names[h], g_budget[h]);
    return n <= g_budget[h];
}

static void rstr(ID3D11Resource *r, char *o, size_t n)
{
    if (!r) { snprintf(o, n, "NULL"); return; }
    D3D11_RESOURCE_DIMENSION dim; ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D *t = (ID3D11Texture2D *)r; D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(t, &d);
        snprintf(o, n, "%p{tex2d %ux%u fmt=%u mips=%u arr=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x}", (void *)r, d.Width, d.Height, d.Format,
                 d.MipLevels, d.ArraySize, d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
        snprintf(o, n, "%p{buffer %u B usage=%u bind=0x%x cpu=0x%x misc=0x%x}", (void *)r, d.ByteWidth, d.Usage, d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
    } else snprintf(o, n, "%p{dim=%d}", (void *)r, dim);
}
static const char *guid(const GUID *g, char *b)
{
    snprintf(b, 40, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
             g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}

/* ---- originaux ---- */
static struct {
    HRESULT (STDMETHODCALLTYPE *tex)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
    HRESULT (STDMETHODCALLTYPE *tex1)(ID3D11Device3 *, const D3D11_TEXTURE2D_DESC1 *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D1 **);
    HRESULT (STDMETHODCALLTYPE *rtv)(ID3D11Device *, ID3D11Resource *, const D3D11_RENDER_TARGET_VIEW_DESC *, ID3D11RenderTargetView **);
    HRESULT (STDMETHODCALLTYPE *rtv1)(ID3D11Device3 *, ID3D11Resource *, const D3D11_RENDER_TARGET_VIEW_DESC1 *, ID3D11RenderTargetView1 **);
    HRESULT (STDMETHODCALLTYPE *srv)(ID3D11Device *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC *, ID3D11ShaderResourceView **);
    HRESULT (STDMETHODCALLTYPE *srv1)(ID3D11Device3 *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *, ID3D11ShaderResourceView1 **);
    HRESULT (STDMETHODCALLTYPE *uav)(ID3D11Device *, ID3D11Resource *, const D3D11_UNORDERED_ACCESS_VIEW_DESC *, ID3D11UnorderedAccessView **);
    HRESULT (STDMETHODCALLTYPE *uav1)(ID3D11Device3 *, ID3D11Resource *, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *, ID3D11UnorderedAccessView1 **);
    HRESULT (STDMETHODCALLTYPE *opensh)(ID3D11Device *, HANDLE, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *buf)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
    void (STDMETHODCALLTYPE *copyres)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
    void (STDMETHODCALLTYPE *copysub)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
    HRESULT (STDMETHODCALLTYPE *map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
    void (STDMETHODCALLTYPE *unmap)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
    void (STDMETHODCALLTYPE *draw)(ID3D11DeviceContext *, UINT, UINT);
    void (STDMETHODCALLTYPE *drawidx)(ID3D11DeviceContext *, UINT, UINT, INT);
    void (STDMETHODCALLTYPE *dispatch)(ID3D11DeviceContext *, UINT, UINT, UINT);
    void (STDMETHODCALLTYPE *flush)(ID3D11DeviceContext *);
    void (STDMETHODCALLTYPE *updsub)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
    void (STDMETHODCALLTYPE *clearrtv)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
    HRESULT (STDMETHODCALLTYPE *texqi)(ID3D11Texture2D *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *vpblt)(ID3D11VideoContext *, ID3D11VideoProcessor *, ID3D11VideoProcessorOutputView *, UINT, UINT, const D3D11_VIDEO_PROCESSOR_STREAM *);
} O;

static void d2str(const D3D11_TEXTURE2D_DESC *d, char *o, size_t n)
{ snprintf(o, n, "%ux%u fmt=%u mips=%u arr=%u samples=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", d->Width, d->Height, d->Format, d->MipLevels, d->ArraySize, d->SampleDesc.Count, d->Usage, d->BindFlags, d->CPUAccessFlags, d->MiscFlags); }

static HRESULT STDMETHODCALLTYPE h_tex(ID3D11Device *s, const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Texture2D **o)
{
    HRESULT hr = O.tex(s, d, i, o);
    if (bud(H_TEX)) { char b[256]; d2str(d, b, sizeof b); plh_trace_msg("CreateTexture2D(%s init=%d) -> 0x%08lx %p", b, i != NULL, hr, o ? (void *)*o : NULL); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_tex1(ID3D11Device3 *s, const D3D11_TEXTURE2D_DESC1 *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Texture2D1 **o)
{
    HRESULT hr = O.tex1(s, d, i, o);
    if (bud(H_TEX1)) plh_trace_msg("CreateTexture2D1(%ux%u fmt=%u mips=%u arr=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x layout=%u init=%d) -> 0x%08lx %p",
        d->Width, d->Height, d->Format, d->MipLevels, d->ArraySize, d->Usage, d->BindFlags, d->CPUAccessFlags, d->MiscFlags, d->TextureLayout, i != NULL, hr, o ? (void *)*o : NULL);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_buf(ID3D11Device *s, const D3D11_BUFFER_DESC *d, const D3D11_SUBRESOURCE_DATA *i, ID3D11Buffer **o)
{
    HRESULT hr = O.buf(s, d, i, o);
    if (bud(H_BUF)) plh_trace_msg("CreateBuffer(%u B usage=%u bind=0x%x cpu=0x%x misc=0x%x) -> 0x%08lx", d->ByteWidth, d->Usage, d->BindFlags, d->CPUAccessFlags, d->MiscFlags, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_rtv(ID3D11Device *s, ID3D11Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC *d, ID3D11RenderTargetView **o)
{
    HRESULT hr = O.rtv(s, r, d, o);
    if (bud(H_RTV)) { char b[300]; rstr(r, b, sizeof b); plh_trace_msg("CreateRenderTargetView(%s fmt=%d dim=%d) -> 0x%08lx", b, d ? (int)d->Format : -1, d ? (int)d->ViewDimension : -1, hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_rtv1(ID3D11Device3 *s, ID3D11Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC1 *d, ID3D11RenderTargetView1 **o)
{
    HRESULT hr = O.rtv1(s, r, d, o);
    if (bud(H_RTV1)) { char b[300]; rstr(r, b, sizeof b);
        plh_trace_msg("CreateRenderTargetView1(%s fmt=%d dim=%d plane=%u) -> 0x%08lx", b, d ? (int)d->Format : -1, d ? (int)d->ViewDimension : -1, d ? d->Texture2D.PlaneSlice : 0, hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_srv(ID3D11Device *s, ID3D11Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC *d, ID3D11ShaderResourceView **o)
{
    HRESULT hr = O.srv(s, r, d, o);
    if (bud(H_SRV)) { char b[300]; rstr(r, b, sizeof b); plh_trace_msg("CreateShaderResourceView(%s fmt=%d dim=%d) -> 0x%08lx", b, d ? (int)d->Format : -1, d ? (int)d->ViewDimension : -1, hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_srv1(ID3D11Device3 *s, ID3D11Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *d, ID3D11ShaderResourceView1 **o)
{
    HRESULT hr = O.srv1(s, r, d, o);
    if (bud(H_SRV1)) { char b[300]; rstr(r, b, sizeof b);
        plh_trace_msg("CreateShaderResourceView1(%s fmt=%d dim=%d plane=%u) -> 0x%08lx", b, d ? (int)d->Format : -1, d ? (int)d->ViewDimension : -1, d ? d->Texture2D.PlaneSlice : 0, hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_uav(ID3D11Device *s, ID3D11Resource *r, const D3D11_UNORDERED_ACCESS_VIEW_DESC *d, ID3D11UnorderedAccessView **o)
{
    HRESULT hr = O.uav(s, r, d, o);
    if (bud(H_UAV)) { char b[300]; rstr(r, b, sizeof b); plh_trace_msg("CreateUnorderedAccessView(%s fmt=%d dim=%d) -> 0x%08lx", b, d ? (int)d->Format : -1, d ? (int)d->ViewDimension : -1, hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_uav1(ID3D11Device3 *s, ID3D11Resource *r, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *d, ID3D11UnorderedAccessView1 **o)
{
    HRESULT hr = O.uav1(s, r, d, o);
    if (bud(H_UAV1)) { char b[300]; rstr(r, b, sizeof b);
        plh_trace_msg("CreateUnorderedAccessView1(%s fmt=%d dim=%d plane=%u) -> 0x%08lx", b, d ? (int)d->Format : -1, d ? (int)d->ViewDimension : -1, d ? d->Texture2D.PlaneSlice : 0, hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_opensh(ID3D11Device *s, HANDLE h, REFIID iid, void **o)
{
    HRESULT hr = O.opensh(s, h, iid, o);
    if (bud(H_OPENSH)) { char g[40]; plh_trace_msg("OpenSharedResource(handle=%p iid=%s) -> 0x%08lx", h, guid(iid, g), hr); }
    return hr;
}
static void STDMETHODCALLTYPE h_copyres(ID3D11DeviceContext *s, ID3D11Resource *d, ID3D11Resource *r)
{
    if (bud(H_COPYRES)) { char a[300], b[300]; rstr(d, a, sizeof a); rstr(r, b, sizeof b); plh_trace_msg("CopyResource(dst=%s src=%s)", a, b); }
    O.copyres(s, d, r);
}
static void STDMETHODCALLTYPE h_copysub(ID3D11DeviceContext *s, ID3D11Resource *d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource *r, UINT ss, const D3D11_BOX *bx)
{
    if (bud(H_COPYSUB)) { char a[300], b[300]; rstr(d, a, sizeof a); rstr(r, b, sizeof b);
        plh_trace_msg("CopySubresourceRegion(dst=%s sub=%u at=%u,%u,%u src=%s sub=%u box=%s)", a, ds, x, y, z, b, ss, bx ? "yes" : "NULL");
        if (bx) plh_trace_msg("   box %u,%u,%u - %u,%u,%u", bx->left, bx->top, bx->front, bx->right, bx->bottom, bx->back); }
    O.copysub(s, d, ds, x, y, z, r, ss, bx);
}
static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; while (n--) { h ^= *p++; h *= 1099511628211ull; } return h; }
static HRESULT STDMETHODCALLTYPE h_map(ID3D11DeviceContext *s, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT fl, D3D11_MAPPED_SUBRESOURCE *m)
{
    HRESULT hr = O.map(s, r, sub, t, fl, m);
    if (bud(H_MAP)) {
        char b[300]; rstr(r, b, sizeof b);
        plh_trace_msg("Map(%s sub=%u type=%d flags=0x%x) -> 0x%08lx pData=%p rowpitch=%u depthpitch=%u", b, sub, t, fl, hr, SUCCEEDED(hr) ? m->pData : NULL,
                      SUCCEEDED(hr) ? m->RowPitch : 0, SUCCEEDED(hr) ? m->DepthPitch : 0);
        D3D11_RESOURCE_DIMENSION dim; ID3D11Resource_GetType(r, &dim);
        if (SUCCEEDED(hr) && m->pData && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && (t == D3D11_MAP_READ || t == D3D11_MAP_READ_WRITE)) {
            D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
            const uint8_t *p = m->pData; size_t pitch = m->RowPitch;
            size_t ny = d.Height, nuv = d.Format == DXGI_FORMAT_NV12 ? d.Height / 2 : 0, zy = 0, zuv = 0, cy = 0, cuv = 0;
            size_t rowb = d.Format == DXGI_FORMAT_NV12 ? d.Width : pitch < d.Width * 4 ? pitch : d.Width * 4;
            for (size_t y = 0; y < ny; y++) for (size_t x = 0; x < rowb; x += 7) { cy++; zy += p[y * pitch + x] == 0; }
            for (size_t y = 0; y < nuv; y++) for (size_t x = 0; x < rowb; x += 7) { cuv++; zuv += p[(ny + y) * pitch + x] == 0; }
            plh_trace_msg("   content: null bytes plane1 %.1f%% (%zu samples)%s%.1f%% ; fnv(4KB)=%016llx ; first 16=%02x%02x%02x%02x %02x%02x%02x%02x", cy ? 100.0 * zy / cy : 0.0, cy,
                          nuv ? "  UV plane " : "  (no UV plane) ", nuv && cuv ? 100.0 * zuv / cuv : 0.0, (unsigned long long)fnv(p, 4096), p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }
    return hr;
}
static void STDMETHODCALLTYPE h_unmap(ID3D11DeviceContext *s, ID3D11Resource *r, UINT sub)
{
    if (bud(H_UNMAP)) { char b[300]; rstr(r, b, sizeof b); plh_trace_msg("Unmap(%s sub=%u)", b, sub); }
    O.unmap(s, r, sub);
}
static void STDMETHODCALLTYPE h_draw(ID3D11DeviceContext *s, UINT n, UINT st)
{
    if (bud(H_DRAW)) {
        ID3D11RenderTargetView *rt[2] = { 0 }; ID3D11DepthStencilView *dsv = NULL;
        ID3D11DeviceContext_OMGetRenderTargets(s, 2, rt, &dsv);
        char b[300] = "(no target)";
        if (rt[0]) { ID3D11Resource *r = NULL; ID3D11RenderTargetView_GetResource(rt[0], &r); rstr(r, b, sizeof b); D3D11_RENDER_TARGET_VIEW_DESC vd; ID3D11RenderTargetView_GetDesc(rt[0], &vd);
                     size_t k = strlen(b); snprintf(b + k, sizeof b - k, " viewfmt=%d", vd.Format); if (r) ID3D11Resource_Release(r); }
        ID3D11ShaderResourceView *sv = NULL; char c[300] = "(no SRV)";
        ID3D11DeviceContext_PSGetShaderResources(s, 0, 1, &sv);
        if (sv) { ID3D11Resource *r = NULL; ID3D11ShaderResourceView_GetResource(sv, &r); rstr(r, c, sizeof c); if (r) ID3D11Resource_Release(r); ID3D11ShaderResourceView_Release(sv); }
        D3D11_VIEWPORT vp; UINT nv = 1; ID3D11DeviceContext_RSGetViewports(s, &nv, &vp);
        plh_trace_msg("Draw(%u,%u) cible0=%s ; PS srv0=%s ; viewport %.0fx%.0f", n, st, b, c, nv ? vp.Width : 0, nv ? vp.Height : 0);
        for (int i = 0; i < 2; i++) if (rt[i]) ID3D11RenderTargetView_Release(rt[i]);
        if (dsv) ID3D11DepthStencilView_Release(dsv);
    }
    O.draw(s, n, st);
}
static void STDMETHODCALLTYPE h_drawidx(ID3D11DeviceContext *s, UINT n, UINT a, INT b)
{
    if (bud(H_DRAWIDX)) plh_trace_msg("DrawIndexed(%u,%u,%d)", n, a, b);
    O.drawidx(s, n, a, b);
}
static void STDMETHODCALLTYPE h_dispatch(ID3D11DeviceContext *s, UINT x, UINT y, UINT z)
{
    if (bud(H_DISPATCH)) plh_trace_msg("Dispatch(%u,%u,%u)", x, y, z);
    O.dispatch(s, x, y, z);
}
static void STDMETHODCALLTYPE h_flush(ID3D11DeviceContext *s)
{
    if (bud(H_FLUSH)) plh_trace_msg("Flush()");
    O.flush(s);
}
static void STDMETHODCALLTYPE h_updsub(ID3D11DeviceContext *s, ID3D11Resource *r, UINT sub, const D3D11_BOX *bx, const void *d, UINT rp, UINT dp)
{
    if (bud(H_UPDSUB)) { char b[300]; rstr(r, b, sizeof b); plh_trace_msg("UpdateSubresource(%s sub=%u box=%s rowpitch=%u)", b, sub, bx ? "yes" : "NULL", rp); }
    O.updsub(s, r, sub, bx, d, rp, dp);
}
static void STDMETHODCALLTYPE h_clearrtv(ID3D11DeviceContext *s, ID3D11RenderTargetView *v, const FLOAT c[4])
{
    if (bud(H_CLEARRTV)) plh_trace_msg("ClearRenderTargetView(%p %.2f,%.2f,%.2f,%.2f)", (void *)v, c[0], c[1], c[2], c[3]);
    O.clearrtv(s, v, c);
}
static HRESULT STDMETHODCALLTYPE h_texqi(ID3D11Texture2D *s, REFIID iid, void **o)
{
    HRESULT hr = O.texqi(s, iid, o);
    if (bud(H_QI)) { char g[40]; plh_trace_msg("Texture2D(%p)::QueryInterface(%s) -> 0x%08lx", (void *)s, guid(iid, g), hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_vpblt(ID3D11VideoContext *s, ID3D11VideoProcessor *vp, ID3D11VideoProcessorOutputView *ov, UINT f, UINT n, const D3D11_VIDEO_PROCESSOR_STREAM *st)
{
    HRESULT hr = O.vpblt(s, vp, ov, f, n, st);
    if (bud(H_VPBLT)) plh_trace_msg("VideoProcessorBlt(streams=%u) -> 0x%08lx", n, hr);
    return hr;
}

static void patch(void **vt, size_t idx, void *fn, void *orig)
{
    DWORD old;
    if (vt[idx] == fn) return;
    if (!VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old)) return;
    if (orig) *(void **)orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}
#define SLOT(T, m) (offsetof(T, m) / sizeof(void *))

void plh_trace_install(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex)
{
    if (!plh_trace_enabled()) return;
    static volatile LONG once_dev, once_ctx, once_tex, once_vc;
    if (dev && InterlockedCompareExchange(&once_dev, 1, 0) == 0) {
        void **vt = *(void ***)dev;
        patch(vt, SLOT(ID3D11DeviceVtbl, CreateTexture2D), h_tex, &O.tex);
        patch(vt, SLOT(ID3D11DeviceVtbl, CreateBuffer), h_buf, &O.buf);
        patch(vt, SLOT(ID3D11DeviceVtbl, CreateRenderTargetView), h_rtv, &O.rtv);
        patch(vt, SLOT(ID3D11DeviceVtbl, CreateShaderResourceView), h_srv, &O.srv);
        patch(vt, SLOT(ID3D11DeviceVtbl, CreateUnorderedAccessView), h_uav, &O.uav);
        patch(vt, SLOT(ID3D11DeviceVtbl, OpenSharedResource), h_opensh, &O.opensh);
        ID3D11Device3 *d3 = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, (void **)&d3))) {
            void **v3 = *(void ***)d3;
            patch(v3, SLOT(ID3D11Device3Vtbl, CreateTexture2D1), h_tex1, &O.tex1);
            patch(v3, SLOT(ID3D11Device3Vtbl, CreateRenderTargetView1), h_rtv1, &O.rtv1);
            patch(v3, SLOT(ID3D11Device3Vtbl, CreateShaderResourceView1), h_srv1, &O.srv1);
            patch(v3, SLOT(ID3D11Device3Vtbl, CreateUnorderedAccessView1), h_uav1, &O.uav1);
            ID3D11Device3_Release(d3);
        }
        plh_trace_msg("tracer: device vtable patched (device=%p); earlier calls are not seen", (void *)dev);
    }
    if (ctx && InterlockedCompareExchange(&once_ctx, 1, 0) == 0) {
        void **vt = *(void ***)ctx;
        patch(vt, SLOT(ID3D11DeviceContextVtbl, CopyResource), h_copyres, &O.copyres);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, CopySubresourceRegion), h_copysub, &O.copysub);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, Map), h_map, &O.map);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, Unmap), h_unmap, &O.unmap);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, Draw), h_draw, &O.draw);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, DrawIndexed), h_drawidx, &O.drawidx);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, Dispatch), h_dispatch, &O.dispatch);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, Flush), h_flush, &O.flush);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, UpdateSubresource), h_updsub, &O.updsub);
        patch(vt, SLOT(ID3D11DeviceContextVtbl, ClearRenderTargetView), h_clearrtv, &O.clearrtv);
        plh_trace_msg("tracer: immediate context vtable patched (ctx=%p)", (void *)ctx);
    }
    if (ctx && InterlockedCompareExchange(&once_vc, 1, 0) == 0) {
        ID3D11VideoContext *vc = NULL;
        if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11VideoContext, (void **)&vc))) {
            patch(*(void ***)vc, SLOT(ID3D11VideoContextVtbl, VideoProcessorBlt), h_vpblt, &O.vpblt);
            ID3D11VideoContext_Release(vc);
            plh_trace_msg("tracer: ID3D11VideoContext::VideoProcessorBlt patched");
        }
    }
    if (tex && InterlockedCompareExchange(&once_tex, 1, 0) == 0) {
        patch(*(void ***)tex, SLOT(ID3D11Texture2DVtbl, QueryInterface), h_texqi, &O.texqi);
        plh_trace_msg("tracer: Texture2D::QueryInterface patched");
    }
}
