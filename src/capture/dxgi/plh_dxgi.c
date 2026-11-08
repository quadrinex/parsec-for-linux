/*
 * plh_dxgi.c — dxgi.dll proxy. Owner: capture.
 * Delegates everything to DXVK's dxgi (renamed dxgi_dxvk.dll in the dedicated prefix) and provides
 * IDXGIOutput1::DuplicateOutput / IDXGIOutput5::DuplicateOutput1 by replacing these two
 * entries of the DXVK output class vtable (in process memory only),
 * wired to the plh-bridge capture service (CPU copy shm -> UpdateSubresource).
 * Only public Windows APIs are implemented; no Parsec binary is touched.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include "../../bridge/client/plh_client.h"
#include "plh_trace.h"

#define MAXDUP 16
#define MAXRECT 64
#define CURMAX 256

static void plog(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    b[n] = '\n'; b[n + 1] = 0;
    fputs("plh-dxgi: ", stderr); fputs(b, stderr); fflush(stderr);
}

/* ---------- real DXVK ---------- */
typedef HRESULT (WINAPI *pf_cf)(REFIID, void **);
typedef HRESULT (WINAPI *pf_cf2)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *pf_v)(void);
typedef HRESULT (WINAPI *pf_dbg)(UINT, REFIID, void **);
static HMODULE g_real;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_real(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    wcscpy(path + n, L"\\dxgi_dxvk.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) plog("ERROR: %ls not found (err %lu)", path, GetLastError());
    return TRUE;
}
static FARPROC real(const char *name)
{
    InitOnceExecuteOnce(&g_once, load_real, NULL, NULL);
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

/* ---------- bridge ---------- */
struct dup;
static void vout_unref(void);
static void redirect_clear(void);
static int is_placeholder(void *o);
static CRITICAL_SECTION g_lock;      /* protects g_dups, g_conn */
static struct dup *g_dups[MAXDUP];
static struct plh_conn *g_conn;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK lock_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_lock); return TRUE; }
static void glock(void) { InitOnceExecuteOnce(&g_lock_once, lock_init, NULL, NULL); EnterCriticalSection(&g_lock); }

struct dup {
    IDXGIOutputDuplication iface;
    LONG ref;
    struct plh_cap_open_reply rep;
    struct plh_shm_map shm;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D10Multithread *mt;
    ID3D11Texture2D *tex;
    DXGI_OUTDUPL_DESC desc;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int got_first, have_frame, mouse_pending, acquired, closing;
    uint32_t pend_slot, pend_w, pend_h, pend_stride, pend_nrect, accumulated;
    RECT rects[MAXRECT], cur_rects[MAXRECT];
    uint32_t cur_nrect;
    LARGE_INTEGER last_present, last_mouse;
    int cur_visible; int cur_x, cur_y, cur_hot_x, cur_hot_y; uint32_t cur_w, cur_h;
    uint8_t *shape; int shape_known, shape_dirty;
    uint32_t rep_state;
    int redirect;                  /* F2: an input redirect was pushed for this duplication */
    int virt;                      /* duplicates the bridge virtual output (holds a vout reference) */
};
#define DUP(p) ((struct dup *)(p))

/* Cursor-path counters, logged every 5 s (plog) so a real Parsec session shows which cursor calls it makes. */
static struct { volatile LONG acq, acq_mouse, acq_vis, acq_shape_sz, shape_calls, shape_ok, shape_more, ev_cursor, ev_shape, ev_hidden, logged; ULONGLONG last; } g_cs;
static void cursor_stats(void)
{
    ULONGLONG now = GetTickCount64();
    if (!g_cs.last) g_cs.last = now;
    if (now - g_cs.last < 5000 || !g_cs.acq) return;
    g_cs.last = now;
    plog("cursor stats: acquire=%ld with_mouse_update=%ld visible=%ld shape_size_reported=%ld | GetFramePointerShape calls=%ld ok=%ld more_data=%ld | bridge events cursor=%ld new_shape=%ld hidden=%ld",
         g_cs.acq, g_cs.acq_mouse, g_cs.acq_vis, g_cs.acq_shape_sz, g_cs.shape_calls, g_cs.shape_ok, g_cs.shape_more, g_cs.ev_cursor, g_cs.ev_shape, g_cs.ev_hidden);
}

static void release_slot(struct dup *d, uint32_t slot)
{
    struct plh_cap_release r = { d->rep.stream_id, slot };
    if (g_conn) plh_conn_send(g_conn, PLH_MSG_CAP_RELEASE, &r, sizeof r);
}

static void on_event(void *u, const struct plh_msg_hdr *h, const void *p)
{
    (void)u;
    uint32_t sid = *(const uint32_t *)p;
    struct dup *d = NULL;
    glock();
    for (int i = 0; i < MAXDUP; i++) if (g_dups[i] && g_dups[i]->rep.stream_id == sid) d = g_dups[i];
    if (!d) {
        if (h->type == PLH_MSG_CAP_FRAME) release_slot(&(struct dup){ .rep = { .stream_id = sid } }, ((const struct plh_cap_frame *)p)->slot);
        LeaveCriticalSection(&g_lock);
        return;
    }
    EnterCriticalSection(&d->cs);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (h->type == PLH_MSG_CAP_FRAME) {
        const struct plh_cap_frame *f = p;
        if (d->have_frame) release_slot(d, d->pend_slot);   /* older: released without copying */
        d->accumulated += 1 + f->dropped;
        d->have_frame = 1; d->pend_slot = f->slot;
        d->pend_w = f->width; d->pend_h = f->height; d->pend_stride = f->stride;
        uint32_t n = f->dirty_count; if (n > MAXRECT) n = 0;
        const struct plh_rect *r = (const struct plh_rect *)(f + 1);
        if (h->size < sizeof *f + n * sizeof *r) n = 0;
        d->pend_nrect = n;
        for (uint32_t i = 0; i < n; i++) d->rects[i] = (RECT){ r[i].left, r[i].top, r[i].right, r[i].bottom };
        d->last_present = now;
        WakeAllConditionVariable(&d->cv);
    } else if (h->type == PLH_MSG_CAP_CURSOR) {
        const struct plh_cap_cursor *c = p;
        InterlockedIncrement(&g_cs.ev_cursor); if (!c->visible) InterlockedIncrement(&g_cs.ev_hidden); if (c->shape_changed) InterlockedIncrement(&g_cs.ev_shape);
        d->cur_visible = (int)c->visible; d->last_mouse = now;
        d->cur_x = c->x - c->hot_x; d->cur_y = c->y - c->hot_y;
        if (c->shape_changed && c->width && c->height && c->width <= CURMAX && c->height <= CURMAX) {
            const uint8_t *src = (const uint8_t *)d->shm.ptr + d->rep.cursor_offset + (uint64_t)(c->shape_serial & 1) * d->rep.cursor_size;
            if (!d->shape) d->shape = malloc(CURMAX * CURMAX * 4);
            if (d->shape) { memcpy(d->shape, src, (size_t)c->width * c->height * 4); d->shape_known = d->shape_dirty = 1; }
            d->cur_w = c->width; d->cur_h = c->height; d->cur_hot_x = c->hot_x; d->cur_hot_y = c->hot_y;
        }
        if (InterlockedIncrement(&g_cs.logged) <= 6)
            plog("cursor event: visible=%d pointer=%d,%d hot=%d,%d shape=%ux%u changed=%d -> DXGI Position(top-left)=%ld,%ld Visible=%d; first px BGRA=%02x%02x%02x%02x",
                 c->visible, c->x, c->y, c->hot_x, c->hot_y, d->cur_w, d->cur_h, c->shape_changed, d->cur_x, d->cur_y, d->cur_visible,
                 d->shape ? d->shape[0] : 0, d->shape ? d->shape[1] : 0, d->shape ? d->shape[2] : 0, d->shape ? d->shape[3] : 0);
        d->mouse_pending = 1;
        WakeAllConditionVariable(&d->cv);
    }
    LeaveCriticalSection(&d->cs);
    LeaveCriticalSection(&g_lock);
}

static struct plh_conn *bridge(void)
{
    glock();
    if (!g_conn) {
        struct plh_conn *c = NULL;
        int r = plh_conn_open(&c, PLH_CLIENT_DXGI, "plh-dxgi", NULL);
        if (r) plog("cannot connect to the bridge (%d): DuplicateOutput will fail", r);
        else {
            plh_conn_set_event_cb(c, on_event, NULL);
            plh_conn_start_reader(c);
            g_conn = c;
        }
    }
    struct plh_conn *c = g_conn;
    LeaveCriticalSection(&g_lock);
    return c;
}

/* ---------- IDXGIOutputDuplication ---------- */
static HRESULT STDMETHODCALLTYPE d_QI(IDXGIOutputDuplication *s, REFIID riid, void **o)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDXGIObject) || IsEqualIID(riid, &IID_IDXGIOutputDuplication)) {
        *o = s; IDXGIOutputDuplication_AddRef(s); return S_OK;
    }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE d_AddRef(IDXGIOutputDuplication *s) { return (ULONG)InterlockedIncrement(&DUP(s)->ref); }
static ULONG STDMETHODCALLTYPE d_Release(IDXGIOutputDuplication *s)
{
    struct dup *d = DUP(s);
    LONG r = InterlockedDecrement(&d->ref);
    if (r) return (ULONG)r;
    glock();
    for (int i = 0; i < MAXDUP; i++) if (g_dups[i] == d) g_dups[i] = NULL;
    LeaveCriticalSection(&g_lock);
    struct plh_cap_stream_ref sr = { d->rep.stream_id, 0 };
    if (g_conn) plh_conn_request(g_conn, PLH_MSG_CAP_CLOSE, &sr, sizeof sr, NULL, 0, NULL, 2000);
    plh_shm_unmap(&d->shm);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    if (d->mt) ID3D10Multithread_Release(d->mt);
    if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
    if (d->dev) ID3D11Device_Release(d->dev);
    if (d->redirect) redirect_clear();
    if (d->virt) vout_unref();
    DeleteCriticalSection(&d->cs);
    free(d->shape); free(d);
    return 0;
}
static HRESULT STDMETHODCALLTYPE d_SetPD(IDXGIOutputDuplication *s, REFGUID g, UINT n, const void *p) { (void)s; (void)g; (void)n; (void)p; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_SetPDI(IDXGIOutputDuplication *s, REFGUID g, const IUnknown *p) { (void)s; (void)g; (void)p; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_GetPD(IDXGIOutputDuplication *s, REFGUID g, UINT *n, void *p) { (void)s; (void)g; (void)n; (void)p; return DXGI_ERROR_NOT_FOUND; }
static HRESULT STDMETHODCALLTYPE d_GetParent(IDXGIOutputDuplication *s, REFIID r, void **p) { (void)s; (void)r; *p = NULL; return E_NOINTERFACE; }
static void STDMETHODCALLTYPE d_GetDesc(IDXGIOutputDuplication *s, DXGI_OUTDUPL_DESC *o) { *o = DUP(s)->desc; }

static HRESULT STDMETHODCALLTYPE d_Acquire(IDXGIOutputDuplication *s, UINT timeout, DXGI_OUTDUPL_FRAME_INFO *fi, IDXGIResource **res)
{
    struct dup *d = DUP(s);
    if (!fi || !res) return E_INVALIDARG;
    memset(fi, 0, sizeof *fi); *res = NULL;
    EnterCriticalSection(&d->cs);
    if (d->acquired) { LeaveCriticalSection(&d->cs); return DXGI_ERROR_INVALID_CALL; }
    ULONGLONG t0 = GetTickCount64();
    while (!d->have_frame && !(d->mouse_pending && d->got_first) && !d->closing) {
        ULONGLONG el = GetTickCount64() - t0;
        if (el >= timeout) { LeaveCriticalSection(&d->cs); return DXGI_ERROR_WAIT_TIMEOUT; }
        SleepConditionVariableCS(&d->cv, &d->cs, (DWORD)(timeout == INFINITE ? INFINITE : timeout - el));
    }
    if (d->closing) { LeaveCriticalSection(&d->cs); return DXGI_ERROR_ACCESS_LOST; }
    if (d->have_frame) {
        const uint8_t *px = (const uint8_t *)d->shm.ptr + d->rep.slot_offset + (uint64_t)d->pend_slot * d->rep.slot_size;
        if (d->pend_w == d->desc.ModeDesc.Width && d->pend_h == d->desc.ModeDesc.Height) {
            if (d->mt) ID3D10Multithread_Enter(d->mt);
            ID3D11DeviceContext_UpdateSubresource(d->ctx, (ID3D11Resource *)d->tex, 0, NULL, px, d->pend_stride, 0);
            if (d->mt) ID3D10Multithread_Leave(d->mt);
        } else plog("frame size %ux%u != %ux%u: ignored", d->pend_w, d->pend_h, d->desc.ModeDesc.Width, d->desc.ModeDesc.Height);
        release_slot(d, d->pend_slot);
        d->have_frame = 0; d->got_first = 1;
        fi->LastPresentTime = d->last_present;
        fi->AccumulatedFrames = d->accumulated; d->accumulated = 0;
        d->cur_nrect = d->pend_nrect;
        memcpy(d->cur_rects, d->rects, d->pend_nrect * sizeof(RECT));
    }
    if (d->mouse_pending) {
        fi->LastMouseUpdateTime = d->last_mouse;
        d->mouse_pending = 0;
    }
    InterlockedIncrement(&g_cs.acq); if (fi->LastMouseUpdateTime.QuadPart) InterlockedIncrement(&g_cs.acq_mouse); if (d->cur_visible) InterlockedIncrement(&g_cs.acq_vis);
    fi->PointerPosition.Visible = d->cur_visible;
    fi->PointerPosition.Position = (POINT){ d->cur_x, d->cur_y };
    if (d->shape_dirty) { fi->PointerShapeBufferSize = d->cur_w * 4 * d->cur_h; InterlockedIncrement(&g_cs.acq_shape_sz); }
    cursor_stats();
    UINT nr = d->cur_nrect ? d->cur_nrect : 1;
    fi->TotalMetadataBufferSize = fi->AccumulatedFrames ? nr * sizeof(RECT) : 0;
    d->acquired = 1;
    LeaveCriticalSection(&d->cs);
    HRESULT qh = ID3D11Texture2D_QueryInterface(d->tex, &IID_IDXGIResource, (void **)res);
    static volatile LONG nq;
    if (plh_trace_enabled() && qh == S_OK && InterlockedIncrement(&nq) <= 3) {
        void *x = NULL; HANDLE sh = NULL;
        HRESULT h1 = ID3D11Texture2D_QueryInterface(d->tex, &IID_IDXGIKeyedMutex, &x); if (x) IUnknown_Release((IUnknown *)x);
        x = NULL; HRESULT h2 = ID3D11Texture2D_QueryInterface(d->tex, &IID_IDXGIResource1, &x); if (x) IUnknown_Release((IUnknown *)x);
        HRESULT h3 = IDXGIResource_GetSharedHandle(*res, &sh);
        plh_trace_msg("Acquire : resource %p ; QI KeyedMutex=0x%08lx IDXGIResource1=0x%08lx GetSharedHandle=0x%08lx handle=%p", (void *)d->tex, h1, h2, h3, sh);
    }
    return qh;
}
static HRESULT STDMETHODCALLTYPE d_Release_Frame(IDXGIOutputDuplication *s)
{
    struct dup *d = DUP(s);
    EnterCriticalSection(&d->cs);
    int a = d->acquired; d->acquired = 0;
    LeaveCriticalSection(&d->cs);
    return a ? S_OK : DXGI_ERROR_INVALID_CALL;
}
static HRESULT STDMETHODCALLTYPE d_DirtyRects(IDXGIOutputDuplication *s, UINT cap, RECT *buf, UINT *req)
{
    struct dup *d = DUP(s);
    if (!req) return E_INVALIDARG;
    EnterCriticalSection(&d->cs);
    HRESULT hr = S_OK;
    UINT n = d->cur_nrect ? d->cur_nrect : 1, need = n * sizeof(RECT);
    *req = need;
    if (!d->acquired) hr = DXGI_ERROR_INVALID_CALL;
    else if (cap < need || !buf) hr = DXGI_ERROR_MORE_DATA;
    else if (d->cur_nrect) memcpy(buf, d->cur_rects, need);
    else buf[0] = (RECT){ 0, 0, (LONG)d->desc.ModeDesc.Width, (LONG)d->desc.ModeDesc.Height };
    LeaveCriticalSection(&d->cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE d_MoveRects(IDXGIOutputDuplication *s, UINT cap, DXGI_OUTDUPL_MOVE_RECT *buf, UINT *req)
{
    (void)cap; (void)buf;
    if (!req) return E_INVALIDARG;
    *req = 0;
    return DUP(s)->acquired ? S_OK : DXGI_ERROR_INVALID_CALL;
}
static HRESULT STDMETHODCALLTYPE d_PtrShape(IDXGIOutputDuplication *s, UINT cap, void *buf, UINT *req, DXGI_OUTDUPL_POINTER_SHAPE_INFO *info)
{
    struct dup *d = DUP(s);
    if (!req || !info) return E_INVALIDARG;
    EnterCriticalSection(&d->cs);
    HRESULT hr = S_OK;
    InterlockedIncrement(&g_cs.shape_calls);
    if (!d->shape_known) { *req = 0; memset(info, 0, sizeof *info); hr = S_OK; }
    else {
        UINT need = d->cur_w * 4 * d->cur_h;
        *req = need;
        if (cap < need || !buf) { hr = DXGI_ERROR_MORE_DATA; InterlockedIncrement(&g_cs.shape_more); }
        else {
            InterlockedIncrement(&g_cs.shape_ok);
            memcpy(buf, d->shape, need);
            info->Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
            info->Width = d->cur_w; info->Height = d->cur_h; info->Pitch = d->cur_w * 4;
            info->HotSpot = (POINT){ d->cur_hot_x, d->cur_hot_y };
            d->shape_dirty = 0;
        }
    }
    LeaveCriticalSection(&d->cs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE d_Map(IDXGIOutputDuplication *s, DXGI_MAPPED_RECT *m) { (void)s; (void)m; return DXGI_ERROR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_Unmap(IDXGIOutputDuplication *s) { (void)s; return DXGI_ERROR_INVALID_CALL; }

static const IDXGIOutputDuplicationVtbl g_dvtbl = {
    d_QI, d_AddRef, d_Release, d_SetPD, d_SetPDI, d_GetPD, d_GetParent, d_GetDesc,
    d_Acquire, d_DirtyRects, d_MoveRects, d_PtrShape, d_Map, d_Unmap, d_Release_Frame,
};

/* ---------- matching DXVK output <-> bridge output ---------- */
/* Windows translation = KWin - T, with T such that all DXVK outputs (Windows view) coincide
 * with the bridge outputs (KWin view): derived from the real layout, never hard-coded. */
static int match_output(IDXGIOutput *out, const struct plh_cap_output *bo, int nb, int *which, int *txo, int *tyo)
{
    DXGI_OUTPUT_DESC me;
    if (FAILED(IDXGIOutput_GetDesc(out, &me))) return 0;
    RECT mr = me.DesktopCoordinates;
    RECT all[16]; int na = 0;
    IDXGIAdapter *ad = NULL;
    if (SUCCEEDED(IDXGIOutput_GetParent(out, &IID_IDXGIAdapter, (void **)&ad))) {
        for (UINT i = 0; i < 16; i++) {
            IDXGIOutput *o = NULL;
            if (IDXGIAdapter_EnumOutputs(ad, i, &o) != S_OK) break;
            DXGI_OUTPUT_DESC dd;
            if (!is_placeholder(o) && SUCCEEDED(IDXGIOutput_GetDesc(o, &dd))) all[na++] = dd.DesktopCoordinates;
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter_Release(ad);
    }
    if (!na) all[na++] = mr;
    for (int t = 0; t < nb; t++) {              /* hypothesis: bo[t] <-> all[0] */
        int tx = bo[t].x - all[0].left, ty = bo[t].y - all[0].top, ok = 1, hit = -1;
        for (int i = 0; i < na && ok; i++) {
            int f = -1;
            for (int j = 0; j < nb; j++)
                if (bo[j].x - tx == all[i].left && bo[j].y - ty == all[i].top &&
                    (int)bo[j].width == all[i].right - all[i].left && (int)bo[j].height == all[i].bottom - all[i].top) f = j;
            if (f < 0) ok = 0;
            else if (all[i].left == mr.left && all[i].top == mr.top) hit = f;
        }
        if (ok && hit >= 0) { *which = hit; if (txo) *txo = tx; if (tyo) *tyo = ty; plog("Windows->KWin translation = (%d,%d)", tx, ty); return 1; }
    }
    /* fallback: rank by increasing position among the outputs, if the counts match */
    return 0;
}

/* ---------- virtual display placeholder (F1) ---------- */
/* When the bridge offers the virtual display (PLH_VIRTUAL_DISPLAY=1) and Wine does not list the real output yet,
 * EnumOutputs appends a placeholder: a pinned second instance of DXVK's output 0 whose GetDesc/GetDesc1 are
 * overridden (rect right of Wine's desktop, device name after the last one). DuplicateOutput on it asks the bridge
 * for the KWin virtual output. Once Wine lists the real output (S5: it follows live) the placeholder is dropped
 * and match_output() maps that DXVK output to the virtual bridge output. */
typedef HRESULT (STDMETHODCALLTYPE *enumout_fn)(IDXGIAdapter *, UINT, IDXGIOutput **);
typedef HRESULT (STDMETHODCALLTYPE *getdesc_fn)(IDXGIOutput *, DXGI_OUTPUT_DESC *);
typedef HRESULT (STDMETHODCALLTYPE *getdesc1_fn)(IDXGIOutput6 *, DXGI_OUTPUT_DESC1 *);
static enumout_fn g_orig_enumout;
static getdesc_fn g_orig_getdesc;
static getdesc1_fn g_orig_getdesc1;
static IDXGIAdapter *g_adapter;               /* pinned DXVK adapter 0 */
static IDXGIOutput *g_ph;                     /* pinned placeholder object */
static struct vd_state { int active; RECT rect; WCHAR name[32]; DWORD hz; int real_listed; } g_vd;  /* guarded by g_lock */
static LONG g_vout_users;
static ULONGLONG g_grace_deadline;             /* GetTickCount64() after which an idle output may be destroyed */
static HANDLE g_grace_timer;                  /* one reusable timer-queue timer */
static int g_destroying;                      /* a VOUT_DESTROY is in flight (sent outside g_lock) */
static LONG g_redirect_users;                 /* F2 duplications currently holding the input redirect */
static volatile LONG g_watch_started;

static int is_placeholder(void *o) { return o && o == (void *)g_ph; }

static HRESULT STDMETHODCALLTYPE my_GetDesc(IDXGIOutput *s, DXGI_OUTPUT_DESC *d)
{
    HRESULT hr = g_orig_getdesc(s, d);
    if (SUCCEEDED(hr) && is_placeholder(s)) {
        glock();
        wcscpy(d->DeviceName, g_vd.name);
        d->DesktopCoordinates = g_vd.rect;
        d->AttachedToDesktop = TRUE;
        d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        d->Monitor = NULL;
        LeaveCriticalSection(&g_lock);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE my_GetDesc1(IDXGIOutput6 *s, DXGI_OUTPUT_DESC1 *d)
{
    HRESULT hr = g_orig_getdesc1(s, d);
    if (SUCCEEDED(hr) && is_placeholder(s)) {
        glock();
        wcscpy(d->DeviceName, g_vd.name);
        d->DesktopCoordinates = g_vd.rect;
        d->AttachedToDesktop = TRUE;
        d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        d->Monitor = NULL;
        LeaveCriticalSection(&g_lock);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE my_EnumOutputs(IDXGIAdapter *s, UINT i, IDXGIOutput **o)
{
    HRESULT hr = g_orig_enumout(s, i, o);
    if (hr != DXGI_ERROR_NOT_FOUND) return hr;
    glock();
    int act = g_vd.active;
    LeaveCriticalSection(&g_lock);
    if (!act) return hr;
    if (i > 0) {                                  /* the placeholder directly follows the last real output */
        IDXGIOutput *prev = NULL;
        if (g_orig_enumout(s, i - 1, &prev) != S_OK) return hr;
        IDXGIOutput_Release(prev);
    }
    glock();
    if (!g_ph) {
        IDXGIOutput *base = NULL;
        if (g_orig_enumout(s, 0, &base) == S_OK) g_ph = base;   /* keep our reference: pinned forever */
    }
    IDXGIOutput *ph = g_ph;
    if (ph) IDXGIOutput_AddRef(ph);
    LeaveCriticalSection(&g_lock);
    if (!ph) return hr;
    *o = ph;
    return S_OK;
}

static int count_monitors_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM p) { (void)m; (void)dc; (void)r; (*(int *)p)++; return TRUE; }

static void vd_registry(const struct vd_state *v, int offered)
{
    static struct vd_state last; static int last_off = -1, last_valid;
    HKEY k;
    if (!offered) {
        if (last_valid || last_off != 1) {
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"Software\\Wine\\PlhVirtualDisplay");
            if (last_valid && RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wine\\PlhMonitorNames", 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
                WCHAR old[40]; _snwprintf(old, 40, L"%ld,%ld", last.rect.left, last.rect.top);
                RegDeleteValueW(k, old); RegCloseKey(k);
            }
            last_valid = 0; last_off = 1;
        }
        return;
    }
    if (last_valid && !memcmp(&last, v, sizeof last)) return;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wine\\PlhVirtualDisplay", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) {
        plog("virtual display: cannot write the registry key (%lu)", GetLastError());
        return;
    }
    DWORD l = (DWORD)v->rect.left, t = (DWORD)v->rect.top, w = (DWORD)(v->rect.right - v->rect.left), h = (DWORD)(v->rect.bottom - v->rect.top);
    DWORD act = v->real_listed ? 1 : 0;
    RegSetValueExW(k, L"DeviceName", 0, REG_SZ, (const BYTE *)v->name, (DWORD)((wcslen(v->name) + 1) * sizeof(WCHAR)));
    RegSetValueExW(k, L"Left", 0, REG_DWORD, (const BYTE *)&l, 4);
    RegSetValueExW(k, L"Top", 0, REG_DWORD, (const BYTE *)&t, 4);
    RegSetValueExW(k, L"Width", 0, REG_DWORD, (const BYTE *)&w, 4);
    RegSetValueExW(k, L"Height", 0, REG_DWORD, (const BYTE *)&h, 4);
    RegSetValueExW(k, L"RefreshHz", 0, REG_DWORD, (const BYTE *)&v->hz, 4);
    RegSetValueExW(k, L"Active", 0, REG_DWORD, (const BYTE *)&act, 4);
    RegCloseKey(k);
    WCHAR nm[40];                                 /* patch 0005 mechanism: monitor name keyed by "Left,Top" */
    _snwprintf(nm, 40, L"%ld,%ld", v->rect.left, v->rect.top);
    if (last_valid && (last.rect.left != v->rect.left || last.rect.top != v->rect.top)) {
        WCHAR old[40]; _snwprintf(old, 40, L"%ld,%ld", last.rect.left, last.rect.top);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wine\\PlhMonitorNames", 0, KEY_WRITE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, old); RegCloseKey(k); }
    }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wine\\PlhMonitorNames", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, nm, 0, REG_SZ, (const BYTE *)L"Virtual display", sizeof L"Virtual display");
        RegCloseKey(k);
    }
    last = *v; last_valid = 1; last_off = 0;
    plog("virtual display registry: %ls %ld,%ld %lux%lu %luHz Active=%lu", v->name, v->rect.left, v->rect.top, w, h, v->hz, act);
}

static int match_output(IDXGIOutput *out, const struct plh_cap_output *bo, int nb, int *which, int *txo, int *tyo);

/* Asks the bridge for the settings and the real layout and updates the placeholder. Never called from the
 * EnumOutputs hook (no recursion); runs at startup and from the watcher thread. */
static void vd_refresh(void)
{
    struct plh_conn *c = bridge();
    if (!c || !g_adapter) return;
    struct plh_display_state st; uint32_t n = 0;
    if (plh_conn_request(c, PLH_MSG_DISPLAY_STATE, NULL, 0, &st, sizeof st, &n, 3000)) return;
    if (!(st.flags & PLH_DS_OFFER)) {
        glock(); g_vd.active = 0; LeaveCriticalSection(&g_lock);
        vd_registry(NULL, 0);
        return;
    }
    int nwine = 0;
    EnumDisplayMonitors(NULL, NULL, count_monitors_cb, (LPARAM)&nwine);
    int real_listed = nwine > (int)st.real_total;   /* Wine lists one more monitor than the real outputs: never offer a second one */
    /* Windows -> KWin translation from the real layout */
    int tx = 0, ty = 0, which = -1, have_t = 0;
    uint8_t lbuf[sizeof(struct plh_cap_output_list) + 16 * sizeof(struct plh_cap_output)];
    uint32_t ll = 0;
    IDXGIOutput *o0 = NULL;
    if (g_orig_enumout(g_adapter, 0, &o0) == S_OK) {
        if (!plh_conn_request(c, PLH_MSG_CAP_LIST_OUTPUTS, NULL, 0, lbuf, sizeof lbuf, &ll, 5000)) {
            const struct plh_cap_output_list *lst = (void *)lbuf;
            have_t = lst->count && match_output(o0, (const void *)(lbuf + sizeof *lst), (int)lst->count, &which, &tx, &ty);
        }
        IDXGIOutput_Release(o0);
    }
    if (!have_t) { plog("virtual display: cannot derive the Windows->KWin translation, placeholder not offered"); glock(); g_vd.active = 0; LeaveCriticalSection(&g_lock); return; }
    struct vd_state v = { 0 };
    int w = (int)st.mode_width, h = (int)st.mode_height;
    int kx = st.real_right, ky = 0;
    if (st.virtual_active) { kx = st.vx; ky = st.vy; w = (int)st.vwidth; h = (int)st.vheight; }
    v.rect = (RECT){ kx - tx, ky - ty, kx - tx + w, ky - ty + h };
    v.hz = st.mode_refresh_mhz / 1000;
    v.real_listed = real_listed;
    _snwprintf(v.name, 32, L"\\\\.\\DISPLAY%d", real_listed ? nwine : nwine + 1);
    v.active = !real_listed;
    glock();
    int was = g_vd.active;
    g_vd = v;
    LeaveCriticalSection(&g_lock);
    if (!was && v.active) plog("virtual display offered: %ls %ld,%ld %dx%d", v.name, v.rect.left, v.rect.top, w, h);
    vd_registry(&v, 1);
}

static DWORD WINAPI vd_watcher(void *p)
{
    (void)p;
    for (;;) { Sleep(1000); vd_refresh(); }
    return 0;
}

static VOID CALLBACK grace_cb(PVOID p, BOOLEAN fired)
{
    (void)p; (void)fired;
    glock();
    if (g_vout_users != 0 || GetTickCount64() < g_grace_deadline || !g_conn || g_destroying) { LeaveCriticalSection(&g_lock); return; }
    struct plh_conn *c = g_conn;
    g_destroying = 1;                             /* vout_ref waits for the end of the send */
    LeaveCriticalSection(&g_lock);
    struct plh_vout_ref ref = { 0, 0 };
    int r = plh_conn_request(c, PLH_MSG_VOUT_DESTROY, &ref, sizeof ref, NULL, 0, NULL, 3000);
    plog("virtual output released after the grace period (%d)", r);
    glock(); g_destroying = 0; LeaveCriticalSection(&g_lock);
}

static void vout_unref(void)
{
    glock();
    if (--g_vout_users <= 0) {
        g_vout_users = 0;
        g_grace_deadline = GetTickCount64() + 3000;
        if (g_grace_timer) ChangeTimerQueueTimer(NULL, g_grace_timer, 3000, 0);
        else CreateTimerQueueTimer(&g_grace_timer, NULL, grace_cb, NULL, 3000, 0, WT_EXECUTEONLYONCE);
    }
    LeaveCriticalSection(&g_lock);
}

/* F2 trigger: the bridge says every real output is off and PLH_VIRTUAL_DISPLAY_AUTO is on */
static int f2_wanted(struct plh_conn *c)
{
    struct plh_display_state st; uint32_t n = 0;
    if (plh_conn_request(c, PLH_MSG_DISPLAY_STATE, NULL, 0, &st, sizeof st, &n, 3000)) return 0;
    return (st.flags & PLH_DS_AUTO) && st.real_on == 0;
}

/* the redirect is shared: cleared only when the last duplication holding it goes away */
static void redirect_clear(void)
{
    glock();
    int last = --g_redirect_users <= 0;
    if (last) g_redirect_users = 0;
    LeaveCriticalSection(&g_lock);
    plog("input redirect %s", last ? "cleared (last holder)" : "kept (another duplication holds it)");
    if (!last) return;
    struct plh_input_redirect_hdr h = { 0, 0 };
    if (g_conn) plh_conn_send(g_conn, PLH_MSG_INPUT_REDIRECT, &h, sizeof h);
}

/* VOUT_CREATE (or join the existing output) and take a reference; reply in *vr */
static int vout_ref(struct plh_conn *c, struct plh_vout_create_reply *vr, uint32_t w, uint32_t h)
{
    for (;;) {                                    /* a pending destroy is cancelled by the user count */
        glock();
        if (!g_destroying) { g_vout_users++; LeaveCriticalSection(&g_lock); break; }
        LeaveCriticalSection(&g_lock);
        Sleep(10);                                /* let an in-flight VOUT_DESTROY finish first */
    }
    struct plh_vout_create rq = { w, h, 0, 0 };
    uint32_t n = 0;
    int r = plh_conn_request(c, PLH_MSG_VOUT_CREATE, &rq, sizeof rq, vr, sizeof *vr, &n, 15000);
    if (r) { plog("VOUT_CREATE failed (%d)", r); glock(); if (g_vout_users > 0) g_vout_users--; LeaveCriticalSection(&g_lock); }
    return r;
}

static HRESULT do_duplicate(IDXGIOutput *out, IUnknown *device, const char *variant, IDXGIOutputDuplication **pdup)
{
    if (!out || !device || !pdup) return E_INVALIDARG;
    *pdup = NULL;
    struct plh_conn *c = bridge();
    if (!c) return DXGI_ERROR_UNSUPPORTED;
    ID3D11Device *dev = NULL;
    if (FAILED(IUnknown_QueryInterface(device, &IID_ID3D11Device, (void **)&dev))) {
        plog("%s: the device is not an ID3D11Device", variant);
        return DXGI_ERROR_UNSUPPORTED;
    }
    uint8_t lbuf[sizeof(struct plh_cap_output_list) + 16 * sizeof(struct plh_cap_output)];
    uint32_t ll = 0;
    if (plh_conn_request(c, PLH_MSG_CAP_LIST_OUTPUTS, NULL, 0, lbuf, sizeof lbuf, &ll, 5000)) { ID3D11Device_Release(dev); return DXGI_ERROR_UNSUPPORTED; }
    const struct plh_cap_output_list *lst = (void *)lbuf;
    const struct plh_cap_output *bo = (void *)(lbuf + sizeof *lst);
    int which = -1, virt = 0, f2 = 0;
    struct plh_cap_output phout;
    if (is_placeholder(out)) {                    /* the "Virtual display" entry: create the KWin output */
        struct plh_vout_create_reply vr;
        if (vout_ref(c, &vr, 0, 0)) { ID3D11Device_Release(dev); return DXGI_ERROR_UNSUPPORTED; }
        memset(&phout, 0, sizeof phout);
        phout.output_id = vr.output_id; phout.x = vr.x; phout.y = vr.y; phout.width = vr.width; phout.height = vr.height;
        phout.refresh_mhz = vr.refresh_mhz; phout.flags = PLH_CAP_OUTPUT_VIRTUAL;
        snprintf(phout.name, sizeof phout.name, "%s", vr.name);
        virt = 1; which = 0;
        bo = &phout;
    } else if (f2_wanted(c)) {                    /* F2: every real screen is off -> show the virtual one instead */
        DXGI_OUTPUT_DESC od;
        uint32_t fw = 0, fh = 0;
        if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od))) {
            fw = (uint32_t)(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
            fh = (uint32_t)(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
        } else memset(&od, 0, sizeof od);
        struct plh_vout_create_reply vr;
        if (vout_ref(c, &vr, fw, fh)) { ID3D11Device_Release(dev); return DXGI_ERROR_UNSUPPORTED; }
        memset(&phout, 0, sizeof phout);
        phout.output_id = vr.output_id; phout.x = vr.x; phout.y = vr.y; phout.width = vr.width; phout.height = vr.height;
        phout.refresh_mhz = vr.refresh_mhz; phout.flags = PLH_CAP_OUTPUT_VIRTUAL;
        snprintf(phout.name, sizeof phout.name, "%s", vr.name);
        virt = 1; which = 0; bo = &phout;
        struct { struct plh_input_redirect_hdr h; struct plh_redirect r; } rd = { { 1, 0 }, {
            { od.DesktopCoordinates.left, od.DesktopCoordinates.top, od.DesktopCoordinates.right, od.DesktopCoordinates.bottom },
            { vr.x, vr.y, vr.x + (int32_t)vr.width, vr.y + (int32_t)vr.height } } };
        if (plh_conn_send(c, PLH_MSG_INPUT_REDIRECT, &rd, sizeof rd) == 0) { f2 = 1; glock(); g_redirect_users++; LeaveCriticalSection(&g_lock); }
        plog("F2: real screens off, %s duplicates the virtual output; input redirect Windows %ld,%ld,%ld,%ld -> KWin %d,%d,%d,%d (sent=%d)",
             variant, rd.r.from.left, rd.r.from.top, rd.r.from.right, rd.r.from.bottom, rd.r.to.left, rd.r.to.top, rd.r.to.right, rd.r.to.bottom, f2);
    } else if (!lst->count || !match_output(out, bo, (int)lst->count, &which, NULL, NULL)) {
        plog("%s: no bridge output matches", variant);
        ID3D11Device_Release(dev);
        return DXGI_ERROR_UNSUPPORTED;
    } else if (bo[which].flags & PLH_CAP_OUTPUT_VIRTUAL) {   /* Wine now lists the real virtual output */
        struct plh_vout_create_reply vr;
        if (vout_ref(c, &vr, 0, 0)) { ID3D11Device_Release(dev); return DXGI_ERROR_UNSUPPORTED; }
        virt = 1;
    }
    struct dup *d = calloc(1, sizeof *d);
    if (!d) { if (f2) redirect_clear(); if (virt) vout_unref(); ID3D11Device_Release(dev); return E_OUTOFMEMORY; }
    d->virt = virt; d->redirect = f2;
    d->iface.lpVtbl = (IDXGIOutputDuplicationVtbl *)&g_dvtbl; d->ref = 1; d->dev = dev;
    InitializeCriticalSection(&d->cs); InitializeConditionVariable(&d->cv);
    struct plh_cap_open op = { bo[which].output_id, PLH_CAP_BACKEND_AUTO, PLH_CAP_F_CURSOR_METADATA, 0, 4, PLH_FMT_BGRA8 };
    uint32_t rl = 0;
    int r = plh_conn_request(c, PLH_MSG_CAP_OPEN, &op, sizeof op, &d->rep, sizeof d->rep, &rl, 120000);
    if (r && r != PLH_E_AGAIN) { plog("%s: CAP_OPEN error %d", variant, r); goto fail; }
    if (plh_shm_map(&d->shm, d->rep.shm_name, d->rep.shm_size)) { plog("%s: shm %s", variant, d->rep.shm_name); goto fail_close; }
    ID3D11Device_GetImmediateContext(dev, &d->ctx);
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(d->ctx, &IID_ID3D10Multithread, (void **)&d->mt)))
        ID3D10Multithread_SetMultithreadProtected(d->mt, TRUE);   /* native D3D11 default */
    D3D11_TEXTURE2D_DESC td = { d->rep.width, d->rep.height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 },
                                D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &d->tex);
    if (FAILED(hr)) { plog("%s: CreateTexture2D 0x%08lx", variant, hr); goto fail_close; }
    plh_trace_install(dev, d->ctx, d->tex);
    d->desc.ModeDesc = (DXGI_MODE_DESC){ d->rep.width, d->rep.height, { bo[which].refresh_mhz ? bo[which].refresh_mhz : 60000, 1000 },
                                         DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED };
    d->desc.Rotation = DXGI_MODE_ROTATION_IDENTITY;
    d->desc.DesktopImageInSystemMemory = FALSE;
    glock();
    int slot = -1;
    for (int i = 0; i < MAXDUP; i++) if (!g_dups[i]) { slot = i; break; }
    if (slot >= 0) g_dups[slot] = d;
    LeaveCriticalSection(&g_lock);
    if (slot < 0) { plog("%s: too many duplications", variant); goto fail_close; }
    plog("%s OK: output %s stream %u %ux%u shm %s (device %p)", variant, bo[which].name, d->rep.stream_id, d->rep.width, d->rep.height, d->rep.shm_name, (void *)dev);
    *pdup = &d->iface;
    return S_OK;
fail_close:
    { struct plh_cap_stream_ref sr = { d->rep.stream_id, 0 }; plh_conn_request(c, PLH_MSG_CAP_CLOSE, &sr, sizeof sr, NULL, 0, NULL, 2000); }
fail:
    plh_shm_unmap(&d->shm);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    if (d->mt) ID3D10Multithread_Release(d->mt);
    if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
    ID3D11Device_Release(dev);
    if (d->redirect) redirect_clear();
    if (d->virt) vout_unref();
    DeleteCriticalSection(&d->cs); free(d);
    return DXGI_ERROR_UNSUPPORTED;
}

/* ---------- DXVK output vtable ---------- */
typedef HRESULT (STDMETHODCALLTYPE *dup_fn)(IDXGIOutput1 *, IUnknown *, IDXGIOutputDuplication **);
typedef HRESULT (STDMETHODCALLTYPE *dup1_fn)(IDXGIOutput5 *, IUnknown *, UINT, UINT, const DXGI_FORMAT *, IDXGIOutputDuplication **);
static dup_fn g_orig_dup;
static dup1_fn g_orig_dup1;

static HRESULT STDMETHODCALLTYPE my_DuplicateOutput(IDXGIOutput1 *s, IUnknown *dev, IDXGIOutputDuplication **o)
{
    plog("IDXGIOutput1::DuplicateOutput(device=%p)", (void *)dev);
    HRESULT hr = do_duplicate((IDXGIOutput *)s, dev, "DuplicateOutput", o);
    plog("DuplicateOutput -> 0x%08lx", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE my_DuplicateOutput1(IDXGIOutput5 *s, IUnknown *dev, UINT flags, UINT n, const DXGI_FORMAT *f, IDXGIOutputDuplication **o)
{
    char b[200] = ""; size_t k = 0;
    for (UINT i = 0; i < n && f && k < sizeof b - 12; i++) k += (size_t)snprintf(b + k, sizeof b - k, " %u", (unsigned)f[i]);
    plog("IDXGIOutput5::DuplicateOutput1(device=%p flags=0x%x formats=%u:%s)", (void *)dev, flags, n, b);
    int ok = 0;
    for (UINT i = 0; i < n && f; i++) if (f[i] == DXGI_FORMAT_B8G8R8A8_UNORM) ok = 1;
    if (!ok) { plog("DuplicateOutput1: BGRA8 not requested -> DXGI_ERROR_UNSUPPORTED"); return DXGI_ERROR_UNSUPPORTED; }
    HRESULT hr = do_duplicate((IDXGIOutput *)s, dev, "DuplicateOutput1", o);
    plog("DuplicateOutput1 -> 0x%08lx", hr);
    return hr;
}

static volatile LONG g_patched;
static void patch_slot(void **vt, size_t idx, void *fn, void **orig)
{
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old)) { plog("VirtualProtect failed"); return; }
    if (orig) *orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}
static void install_patch(IUnknown *factory)
{
    if (g_patched) return;
    IDXGIFactory *f = NULL; IDXGIAdapter *a = NULL; IDXGIOutput *o = NULL; IDXGIOutput1 *o1 = NULL;
    if (FAILED(IUnknown_QueryInterface(factory, &IID_IDXGIFactory, (void **)&f))) return;
    if (SUCCEEDED(IDXGIFactory_EnumAdapters(f, 0, &a)) && SUCCEEDED(IDXGIAdapter_EnumOutputs(a, 0, &o)) &&
        SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput1, (void **)&o1))) {
        if (InterlockedCompareExchange(&g_patched, 1, 0) == 0) {
            void **vt = *(void ***)o1;
            size_t i1 = offsetof(IDXGIOutput1Vtbl, DuplicateOutput) / sizeof(void *);
            patch_slot(vt, i1, (void *)my_DuplicateOutput, (void **)&g_orig_dup);
            IDXGIOutput5 *o5 = NULL;
            if (SUCCEEDED(IDXGIOutput1_QueryInterface(o1, &IID_IDXGIOutput5, (void **)&o5))) {
                void **vt5 = *(void ***)o5;
                size_t i5 = offsetof(IDXGIOutput5Vtbl, DuplicateOutput1) / sizeof(void *);
                patch_slot(vt5, i5, (void *)my_DuplicateOutput1, (void **)&g_orig_dup1);
                IDXGIOutput5_Release(o5);
            }
            patch_slot(vt, offsetof(IDXGIOutputVtbl, GetDesc) / sizeof(void *), (void *)my_GetDesc, (void **)&g_orig_getdesc);
            IDXGIOutput6 *o6 = NULL;
            if (SUCCEEDED(IDXGIOutput1_QueryInterface(o1, &IID_IDXGIOutput6, (void **)&o6))) {
                void **vt6 = *(void ***)o6;
                patch_slot(vt6, offsetof(IDXGIOutput6Vtbl, GetDesc1) / sizeof(void *), (void *)my_GetDesc1, (void **)&g_orig_getdesc1);
                IDXGIOutput6_Release(o6);
            }
            void **avt = *(void ***)a;
            patch_slot(avt, offsetof(IDXGIAdapterVtbl, EnumOutputs) / sizeof(void *), (void *)my_EnumOutputs, (void **)&g_orig_enumout);
            IDXGIAdapter_AddRef(a);
            g_adapter = a;
            plog("DXVK output vtable patched (Output1%s) + GetDesc/GetDesc1 + Adapter::EnumOutputs", g_orig_dup1 ? " + Output5" : "");
            vd_refresh();
            if (g_conn && InterlockedCompareExchange(&g_watch_started, 1, 0) == 0) {
                HANDLE th = CreateThread(NULL, 0, vd_watcher, NULL, 0, NULL);
                if (th) CloseHandle(th);
            }
        }
        IDXGIOutput1_Release(o1);
    }
    if (o) IDXGIOutput_Release(o);
    if (a) IDXGIAdapter_Release(a);
    IDXGIFactory_Release(f);
}

/* ---------- exports ---------- */
HRESULT WINAPI plh_CreateDXGIFactory(REFIID riid, void **pp)
{
    pf_cf fn = (pf_cf)(void *)real("CreateDXGIFactory");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, pp);
    if (SUCCEEDED(hr)) install_patch(*(IUnknown **)pp);
    return hr;
}
HRESULT WINAPI plh_CreateDXGIFactory1(REFIID riid, void **pp)
{
    pf_cf fn = (pf_cf)(void *)real("CreateDXGIFactory1");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, pp);
    if (SUCCEEDED(hr)) install_patch(*(IUnknown **)pp);
    return hr;
}
HRESULT WINAPI plh_CreateDXGIFactory2(UINT flags, REFIID riid, void **pp)
{
    pf_cf2 fn = (pf_cf2)(void *)real("CreateDXGIFactory2");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(flags, riid, pp);
    if (SUCCEEDED(hr)) install_patch(*(IUnknown **)pp);
    return hr;
}
HRESULT WINAPI plh_DXGIDeclareAdapterRemovalSupport(void)
{
    pf_v fn = (pf_v)(void *)real("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI plh_DXGIGetDebugInterface1(UINT flags, REFIID riid, void **pp)
{
    pf_dbg fn = (pf_dbg)(void *)real("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, pp) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *r) { (void)h; (void)reason; (void)r; return TRUE; }
