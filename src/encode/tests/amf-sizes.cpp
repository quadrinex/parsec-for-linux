/* amf-sizes.exe — AMF client exercising several frame sizes in ONE process, like Parsec does when the client
 * switches display (Terminate + Init at another size, same component or a new one).
 * Usage: amf-sizes.exe <avc|hevc> <nv12|bgra> <outprefix> <mode> <WxH[:TEXWxTEXH]>...
 *   mode "new"    : fresh component per size
 *   mode "reinit" : the same component: Terminate(), set FrameSize, Init() again
 *   mode "reinit2": the same component: ReInit(w,h) (AMF API) without Terminate
 * TEXWxTEXH (optional) gives a D3D11 texture size different from the encoder size (crop / pad paths).
 * Each size writes <outprefix>-<i>.bin with 12 frames (IDR at frame 0, forced IDR at frame 6). */
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../third_party/amf/core/Factory.h"
#include "../third_party/amf/components/VideoEncoderVCE.h"
#include "../third_party/amf/components/VideoEncoderHEVC.h"
using namespace amf;
static AMFVariantStruct I64(int64_t v) { AMFVariantStruct s; memset(&s, 0, sizeof s); s.type = AMF_VARIANT_INT64; s.int64Value = v; return s; }
static bool hevc, nv12;
static AMFFactory *f; static AMFContext *ctx; static ID3D11Device *dev; static ID3D11DeviceContext *dc;

static void setup(AMFComponent *enc, int W, int H)
{
    AMFRate fr = { 60, 1 }; AMFSize sz = { W, H }; AMFVariantStruct v;
    if (!hevc) {
        enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, I64(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY));
        enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, I64(8000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, I64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        enc->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, I64(300));
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_RATE; v.rateValue = fr; enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, v);
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_SIZE; v.sizeValue = sz; enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, v);
    } else {
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, I64(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, I64(8000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, I64(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, I64(300));
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_RATE; v.rateValue = fr; enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, v);
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_SIZE; v.sizeValue = sz; enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, v);
    }
}

/* returns number of frames got, or -1 on error */
static int run(AMFComponent *enc, int TW, int TH, FILE *out, int N)
{
    D3D11_TEXTURE2D_DESC d{}; d.Width = TW; d.Height = TH; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr; HRESULT hr = dev->CreateTexture2D(&d, NULL, &tex);
    if (FAILED(hr)) { printf("CreateTexture2D %dx%d 0x%lx\n", TW, TH, (unsigned long)hr); return -1; }
    std::vector<uint8_t> px((size_t)TW * TH * 4);
    int got = 0; AMF_RESULT r;
    for (int i = 0; i < N + 1; i++) {
        if (i < N) {
            if (nv12) {
                for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) px[(size_t)y * TW + x] = (uint8_t)(((x + i * 4) / 8 + y / 8) & 1 ? 200 : 60);
                memset(&px[(size_t)TW * TH], 128, (size_t)TW * TH / 2 + TW);
                dc->UpdateSubresource(tex, 0, NULL, px.data(), TW, 0);
            } else {
                for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) { uint8_t *p = &px[((size_t)y * TW + x) * 4]; uint8_t c = (((x + i * 4) / 8 + y / 8) & 1) ? 220 : 40; p[0] = c; p[1] = (uint8_t)(x * 255 / TW); p[2] = (uint8_t)(y * 255 / TH); p[3] = 255; }
                dc->UpdateSubresource(tex, 0, NULL, px.data(), TW * 4, 0);
            }
            AMFSurface *s = nullptr; r = ctx->CreateSurfaceFromDX11Native(tex, &s, NULL); if (r != AMF_OK) { printf("CreateSurfaceFromDX11Native %d\n", (int)r); tex->Release(); return -1; }
            s->SetPts(i * 166667);
            if (i == 6) s->SetProperty(hevc ? AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE : AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, I64(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            r = enc->SubmitInput(s); s->Release();
            if (r != AMF_OK) { printf("SubmitInput %d frame %d\n", (int)r, i); tex->Release(); return -1; }
        } else enc->Drain();
        for (;;) {
            AMFData *o = nullptr; r = enc->QueryOutput(&o);
            if (r != AMF_OK || !o) break;
            AMFBuffer *b = nullptr; o->QueryInterface(AMFBuffer::IID(), (void **)&b);
            fwrite(b->GetNative(), 1, b->GetSize(), out); got++;
            b->Release(); o->Release();
        }
    }
    tex->Release();
    return got;
}

int main(int argc, char **argv)
{
    if (argc < 6) return 2;
    hevc = !strcmp(argv[1], "hevc"); nv12 = !strcmp(argv[2], "nv12");
    const char *prefix = argv[3], *mode = argv[4];
    HMODULE m = LoadLibraryW(L"amfrt64.dll"); if (!m) { puts("LoadLibrary amfrt64.dll failed"); return 1; }
    auto qv = (AMFQueryVersion_Fn)GetProcAddress(m, "AMFQueryVersion"); auto in = (AMFInit_Fn)GetProcAddress(m, "AMFInit");
    amf_uint64 ver = 0; if (!qv || !in || qv(&ver) != AMF_OK || in(ver, &f) != AMF_OK) { puts("AMFInit failed"); return 1; }
    f->CreateContext(&ctx);
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &dc))) { puts("D3D11CreateDevice failed"); return 1; }
    if (ctx->InitDX11(dev) != AMF_OK) { puts("InitDX11 failed"); return 1; }
    AMFComponent *enc = nullptr; int bad = 0;
    for (int i = 5; i < argc; i++) {
        int W, H, TW, TH; int n = sscanf(argv[i], "%dx%d:%dx%d", &W, &H, &TW, &TH);
        if (n < 2) return 2;
        if (n < 4) { TW = W; TH = H; }
        AMF_RESULT r = AMF_OK;
        bool again = enc && strcmp(mode, "new");
        if (!again) {
            if (enc) { enc->Terminate(); enc->Release(); enc = nullptr; }
            r = f->CreateComponent(ctx, hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &enc);
            if (r == AMF_OK) { setup(enc, W, H); r = enc->Init(nv12 ? AMF_SURFACE_NV12 : AMF_SURFACE_BGRA, W, H); }
        } else if (!strcmp(mode, "reinit")) {
            enc->Terminate(); setup(enc, W, H); r = enc->Init(nv12 ? AMF_SURFACE_NV12 : AMF_SURFACE_BGRA, W, H);
        } else r = enc->ReInit(W, H);
        if (r != AMF_OK) { printf("SIZE %dx%d init -> %d\n", W, H, (int)r); bad++; if (enc) { enc->Release(); enc = nullptr; } continue; }
        char fn[512]; snprintf(fn, sizeof fn, "%s-%d.bin", prefix, i - 5);
        FILE *out = fopen(fn, "wb"); int got = run(enc, TW, TH, out, 12); fclose(out);
        printf("SIZE %dx%d tex %dx%d frames=%d\n", W, H, TW, TH, got);
        if (got != 12) bad++;
    }
    if (enc) { enc->Terminate(); enc->Release(); }
    ctx->Terminate(); ctx->Release();
    return bad;
}
