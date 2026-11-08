/* vdgrace: adversarial driver of the DXGI virtual display placeholder (F1). Lines on stdout are markers the harness
 * (tests/tools/vd-live.sh) reacts to: "SAMPLE <tag>" = the harness counts Virtual-plh outputs now; the probe then waits 1 s.
 * usage: vdgrace regrace | hold | twice
 *   regrace: dup, release, 1.5 s later dup again (inside the 3 s grace), keep it past the first timer, release.
 *   hold:    dup and hold 60 s (the harness SIGKILLs this process: a Parsec crash).
 *   twice:   list outputs twice 2 s apart; the placeholder must show exactly once each time. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <stdio.h>
#include <string.h>

static IDXGIAdapter *ad; static ID3D11Device *dev;
static void sample(const char *t) { printf("SAMPLE %s\n", t); Sleep(1000); }
static int outs(const char *tag, IDXGIOutput **last)
{
    int n = 0, virt = 0;
    for (UINT i = 0;; i++) {
        IDXGIOutput *o = NULL; DXGI_OUTPUT_DESC d;
        if (IDXGIAdapter_EnumOutputs(ad, i, &o) != S_OK) break;
        IDXGIOutput_GetDesc(o, &d);
        printf("OUT %s %u %ls %ld,%ld\n", tag, i, d.DeviceName, d.DesktopCoordinates.left, d.DesktopCoordinates.top);
        if (last) { if (*last) IDXGIOutput_Release(*last); *last = o; } else IDXGIOutput_Release(o);
        n++; (void)virt;
    }
    printf("COUNT %s %d\n", tag, n);
    return n;
}
static IDXGIOutputDuplication *dup_last(IDXGIOutput *out, const char *tag)
{
    IDXGIOutput5 *o5 = NULL; IDXGIOutputDuplication *d = NULL; DXGI_FORMAT f = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput5, (void **)&o5))) { printf("DUP %s noiface\n", tag); return NULL; }
    HRESULT hr = IDXGIOutput5_DuplicateOutput1(o5, (IUnknown *)dev, 0, 1, &f, &d);
    printf("DUP %s hr=0x%08lx\n", tag, (unsigned long)hr);
    IDXGIOutput5_Release(o5);
    return SUCCEEDED(hr) ? d : NULL;
}
static int frames(IDXGIOutputDuplication *d, int ms)
{
    int n = 0;
    for (int t = 0; t < ms / 100; t++) {
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *r = NULL;
        if (IDXGIOutputDuplication_AcquireNextFrame(d, 100, &fi, &r) == S_OK) { n++; IDXGIResource_Release(r); IDXGIOutputDuplication_ReleaseFrame(d); }
    }
    return n;
}
int wmain(int argc, wchar_t **argv)
{
    const wchar_t *mode = argc > 1 ? argv[1] : L"regrace";
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1 *f;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) { puts("FAIL factory"); return 1; }
    IDXGIFactory1_EnumAdapters(f, 0, &ad);
    Sleep(1500);
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, NULL))) { puts("FAIL device"); return 1; }
    IDXGIOutput *last = NULL;
    if (!wcscmp(mode, L"twice")) {
        int a = outs("first", NULL); Sleep(2000); int b = outs("second", NULL);
        printf("TWICE %d %d\n", a, b); return 0;
    }
    outs("before", &last);
    IDXGIOutputDuplication *d1 = dup_last(last, "1");
    if (!d1) return 2;
    printf("FRAMES1 %d\n", frames(d1, 1500));
    if (!wcscmp(mode, L"hold")) { puts("HOLDING"); Sleep(60000); return 0; }
    sample("a_held");
    IDXGIOutputDuplication_Release(d1); puts("RELEASED1");
    Sleep(1500);
    sample("b_in_grace");
    outs("ingrace", &last);                       /* placeholder must still be offered, exactly once */
    IDXGIOutputDuplication *d2 = dup_last(last, "2");
    if (!d2) return 3;
    sample("c_second_dup");
    printf("FRAMES2 %d\n", frames(d2, 4500));      /* outlives the first release's 3 s timer */
    sample("d_after_first_timer");
    IDXGIOutputDuplication_Release(d2); puts("RELEASED2");
    Sleep(5500);
    sample("e_after_second_grace");
    outs("after", NULL);
    return 0;
}
