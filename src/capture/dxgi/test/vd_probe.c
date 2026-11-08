/* vd_probe.c — F1 probe (owner: capture): lists the DXGI outputs, duplicates the "Virtual display" placeholder, receives
 * frames, releases, and watches the output set change. Run in the dedicated prefix with the proxy installed.
 * usage: vd_probe [seconds_to_hold [f2]]   (f2: duplicate output 0, the "default" screen, instead of the placeholder)   prints PH/OUT/FRAMES lines. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <stdio.h>
#include <wchar.h>

static int monitors_cb(HMONITOR m, HDC d, LPRECT r, LPARAM p) { (void)m; (void)d; (void)r; (*(int *)p)++; return 1; }
static int list(IDXGIAdapter *a, const char *tag, IDXGIOutput **last)
{
    int n = 0;
    for (UINT i = 0;; i++) {
        IDXGIOutput *o = NULL;
        if (IDXGIAdapter_EnumOutputs(a, i, &o) != S_OK) break;
        DXGI_OUTPUT_DESC d; IDXGIOutput_GetDesc(o, &d);
        printf("OUT %s %u %ls %ld,%ld,%ld,%ld\n", tag, i, d.DeviceName, d.DesktopCoordinates.left, d.DesktopCoordinates.top,
               d.DesktopCoordinates.right, d.DesktopCoordinates.bottom);
        if (last) { if (*last) IDXGIOutput_Release(*last); *last = o; } else IDXGIOutput_Release(o);
        n++;
    }
    int m = 0; EnumDisplayMonitors(NULL, NULL, monitors_cb, (LPARAM)&m);
    printf("COUNT %s dxgi=%d wine_monitors=%d\n", tag, n, m);
    return n;
}
int main(int argc, char **argv)
{
    int hold = argc > 1 ? atoi(argv[1]) : 3;
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1 *f; if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) { puts("FAIL factory"); return 1; }
    IDXGIAdapter *a; IDXGIFactory1_EnumAdapters(f, 0, &a);
    Sleep(1500);                                   /* let the proxy's first refresh settle */
    IDXGIOutput *last = NULL;
    int n0 = list(a, "before", &last);
    ID3D11Device *dev; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, NULL))) { puts("FAIL device"); return 1; }
    IDXGIOutput5 *o5 = NULL;
    if (argc > 2) { IDXGIOutput_Release(last); IDXGIAdapter_EnumOutputs(a, 0, &last); }
    IDXGIOutput_QueryInterface(last, &IID_IDXGIOutput5, (void **)&o5);
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    IDXGIOutputDuplication *dup = NULL;
    HRESULT hr = IDXGIOutput5_DuplicateOutput1(o5, (IUnknown *)dev, 0, 1, &fmt, &dup);
    printf("DUP hr=0x%08lx\n", hr);
    if (FAILED(hr)) return 2;
    DXGI_OUTDUPL_DESC dd; IDXGIOutputDuplication_GetDesc(dup, &dd);
    printf("DUPDESC %ux%u\n", dd.ModeDesc.Width, dd.ModeDesc.Height);
    int frames = 0;
    for (int t = 0; t < hold * 10; t++) {
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
        hr = IDXGIOutputDuplication_AcquireNextFrame(dup, 100, &fi, &res);
        if (hr == S_OK) { frames++; IDXGIResource_Release(res); IDXGIOutputDuplication_ReleaseFrame(dup); }
        if (t == 15) list(a, "during", NULL);
    }
    printf("FRAMES %d\n", frames);
    Sleep(500);
    list(a, "during2", NULL);
    IDXGIOutputDuplication_Release(dup);
    puts("RELEASED");
    Sleep(1000); list(a, "after1s", NULL);
    Sleep(4500); list(a, "after5s", NULL);
    (void)n0;
    return 0;
}
