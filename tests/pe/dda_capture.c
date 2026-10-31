/* T2 — Capture via DXGI Desktop Duplication (IDXGIOutput1::DuplicateOutput),
 * i.e. the API Parsec uses on Windows (to be confirmed by api-map).
 *
 *   dda_capture.exe [seconds] [windows_output_dir]
 *
 * For each DXGI output: name, DesktopCoordinates, result of DuplicateOutput,
 * number of frames received and fps over the duration, cursor (position, shape), and
 * dump of the last frame as raw PAM (BGRA → RGB) in the output directory,
 * for checking the test pattern with tests/tools/check_pattern.py. */
#define COBJMACROS
#define INITGUID
#include "plh_test.h"
#include <d3d11.h>
#include <dxgi1_2.h>

#define MAXO 8
typedef struct {
    int idx;
    IDXGIOutput1 *out;
    IDXGIOutputDuplication *dup;
    DXGI_OUTPUT_DESC desc;
    HRESULT hr_dup;
    int frames, updates, ptr_seen, ptr_shape_bytes, ptr_x, ptr_y, ptr_vis;
    HRESULT last_err;
    double seconds;
} OutCtx;

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static CRITICAL_SECTION g_cs;
static double g_secs = 10.0;
static wchar_t g_dir[MAX_PATH] = L"";

static void dump_frame(OutCtx *o, IDXGIResource *res) {
    ID3D11Texture2D *tex = NULL, *st = NULL;
    if (FAILED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) return;
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(tex, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MipLevels = 1; d.ArraySize = 1;
    EnterCriticalSection(&g_cs);
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)tex);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\output%d.pam", g_dir, o->idx);
            FILE *f = _wfopen(path, L"wb");
            if (f) {
                fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB\nENDHDR\n", d.Width, d.Height);
                unsigned char *row = malloc(d.Width * 3);
                for (UINT y = 0; y < d.Height; y++) {
                    const unsigned char *s = (const unsigned char *)m.pData + (size_t)y * m.RowPitch;
                    for (UINT x = 0; x < d.Width; x++) { /* B8G8R8A8 expected (DDA format) */
                        row[3 * x] = s[4 * x + 2]; row[3 * x + 1] = s[4 * x + 1]; row[3 * x + 2] = s[4 * x];
                    }
                    fwrite(row, 1, d.Width * 3, f);
                }
                free(row); fclose(f);
                plh_out("dump idx=%d format=%u size=%ux%u file=%ls\n", o->idx, d.Format, d.Width, d.Height, path);
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    LeaveCriticalSection(&g_cs);
    ID3D11Texture2D_Release(tex);
}

static DWORD WINAPI cap_thread(void *p) {
    OutCtx *o = p;
    double t0 = plh_now(), tend = t0 + g_secs;
    int dumped = 0; /* a single CPU copy, halfway through, so as not to distort the fps */
    while (plh_now() < tend) {
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
        HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(o->dup, 100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { o->last_err = hr; break; }
        o->frames++;
        if (fi.LastPresentTime.QuadPart) o->updates++;
        if (fi.LastMouseUpdateTime.QuadPart) {
            o->ptr_seen = 1; o->ptr_vis = fi.PointerPosition.Visible;
            o->ptr_x = fi.PointerPosition.Position.x; o->ptr_y = fi.PointerPosition.Position.y;
        }
        if (fi.PointerShapeBufferSize) {
            void *buf = malloc(fi.PointerShapeBufferSize); UINT req = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
            if (SUCCEEDED(IDXGIOutputDuplication_GetFramePointerShape(o->dup, fi.PointerShapeBufferSize, buf, &req, &si)))
                o->ptr_shape_bytes = (int)req;
            free(buf);
        }
        if (!dumped && fi.LastPresentTime.QuadPart && res && plh_now() > t0 + g_secs / 2) { dump_frame(o, res); dumped = 1; }
        if (res) IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(o->dup);
    }
    o->seconds = plh_now() - t0;
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc > 1) g_secs = _wtof(argv[1]);
    if (argc > 2) wcsncpy(g_dir, argv[2], MAX_PATH - 1);
    else GetTempPathW(MAX_PATH, g_dir);
    int only = argc > 3 ? _wtoi(argv[3]) : -1; /* >= 0: duplicate only this output (D-011) */
    InitializeCriticalSection(&g_cs);
    plh_out("wine_version=%s\n", plh_wine_version() ? plh_wine_version() : "(not Wine)");

    IDXGIFactory1 *fac = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac);
    OK(SUCCEEDED(hr), "CreateDXGIFactory1 hr=0x%08lx", hr);
    if (FAILED(hr)) return 1;
    plh_print_module("dxgi.dll");

    IDXGIAdapter1 *ad = NULL;
    hr = IDXGIFactory1_EnumAdapters1(fac, 0, &ad);
    OK(SUCCEEDED(hr), "EnumAdapters1(0) hr=0x%08lx", hr);
    if (FAILED(hr)) return 1;
    DXGI_ADAPTER_DESC1 ades; IDXGIAdapter1_GetDesc1(ad, &ades);
    plh_out("adapter=%ls vendor=0x%04x device=0x%04x\n", ades.Description, ades.VendorId, ades.DeviceId);

    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
    OK(SUCCEEDED(hr), "D3D11CreateDevice hr=0x%08lx fl=0x%x", hr, fl);
    if (FAILED(hr)) return 1;
    plh_print_module("d3d11.dll");

    OutCtx o[MAXO] = {0}; int n = 0;
    for (UINT i = 0; i < MAXO; i++) {
        IDXGIOutput *out = NULL;
        if (IDXGIAdapter1_EnumOutputs(ad, i, &out) == DXGI_ERROR_NOT_FOUND) break;
        o[n].idx = (int)i;
        IDXGIOutput_GetDesc(out, &o[n].desc);
        RECT r = o[n].desc.DesktopCoordinates;
        plh_out("output idx=%u name=%ls rect=%ld,%ld,%ld,%ld attached=%d\n", i, o[n].desc.DeviceName,
                r.left, r.top, r.right - r.left, r.bottom - r.top, o[n].desc.AttachedToDesktop);
        if (only >= 0 && (int)i != only) { IDXGIOutput_Release(out); o[n].hr_dup = E_FAIL; n++; continue; }
        hr = IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&o[n].out);
        IDXGIOutput_Release(out);
        if (FAILED(hr)) { plh_out("output1 idx=%u hr=0x%08lx\n", i, hr); continue; }
        o[n].hr_dup = IDXGIOutput1_DuplicateOutput(o[n].out, (IUnknown *)g_dev, &o[n].dup);
        plh_out("dup idx=%u hr=0x%08lx\n", i, o[n].hr_dup);
        n++;
    }
    plh_out("outputs=%d\n", n);
    HANDLE th[MAXO]; int nt = 0;
    for (int i = 0; i < n; i++)
        if (SUCCEEDED(o[i].hr_dup)) th[nt++] = CreateThread(NULL, 0, cap_thread, &o[i], 0, NULL);
    if (nt) WaitForMultipleObjects(nt, th, TRUE, INFINITE);
    for (int i = 0; i < n; i++) {
        if (FAILED(o[i].hr_dup)) continue;
        plh_out("frames idx=%d n=%d updates=%d secs=%.2f fps=%.1f last_err=0x%08lx\n", o[i].idx, o[i].frames,
                o[i].updates, o[i].seconds, o[i].seconds > 0 ? o[i].updates / o[i].seconds : 0.0, o[i].last_err);
        plh_out("pointer idx=%d seen=%d visible=%d pos=%d,%d shape_bytes=%d\n", o[i].idx, o[i].ptr_seen,
                o[i].ptr_vis, o[i].ptr_x, o[i].ptr_y, o[i].ptr_shape_bytes);
    }
    return 0;
}
