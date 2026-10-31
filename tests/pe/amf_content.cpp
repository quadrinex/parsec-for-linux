/* amf_content.exe — critique: AMF client (public API) whose input is a D3D11 BGRA texture with KNOWN
 * content: red/green/blue/white quadrants (R,G,B,W) + a 40 px black bar in the band y>=680 that advances
 * by 10 px/frame. The test decodes the stream and compares colors (a valid but black/green/R<->B stream fails).
 * Usage: amf_content.exe <avc|hevc> <output.bin> [frames=60]   (PLH_BRIDGE_DIR inherited from the environment)
 * Standard output: "init=<code>", "lat_ms=<p50> <max>", "RESULT frames=<n> idr=<n>"; exit code 3 on init/submit failure
 * (clean error, never a hang), 0 if N frames received. */
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include "../../src/encode/third_party/amf/core/Factory.h"
#include "../../src/encode/third_party/amf/components/VideoEncoderVCE.h"
#include "../../src/encode/third_party/amf/components/VideoEncoderHEVC.h"
using namespace amf;
static AMFVariantStruct I64(int64_t v) { AMFVariantStruct s; memset(&s, 0, sizeof s); s.type = AMF_VARIANT_INT64; s.int64Value = v; return s; }
static double now_ms() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0 * (double)c.QuadPart / (double)f.QuadPart; }
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    bool hevc = !strcmp(argv[1], "hevc"); int N = argc > 3 ? atoi(argv[3]) : 60, W = 1280, H = 720;
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE m = LoadLibraryW(L"amfrt64.dll"); if (!m) { puts("init=loadlib"); return 3; }
    auto qv = (AMFQueryVersion_Fn)GetProcAddress(m, "AMFQueryVersion"); auto in = (AMFInit_Fn)GetProcAddress(m, "AMFInit");
    amf_uint64 ver = 0; AMFFactory *f = nullptr;
    if (!qv || !in || qv(&ver) != AMF_OK || in(ver, &f) != AMF_OK) { puts("init=amfinit"); return 3; }
    AMFContext *ctx = nullptr; f->CreateContext(&ctx);
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *dc = nullptr; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &dc))) { puts("init=d3d"); return 3; }
    AMF_RESULT r = ctx->InitDX11(dev); if (r != AMF_OK) { printf("init=initdx11:%d\n", (int)r); return 3; }
    AMFComponent *enc = nullptr;
    r = f->CreateComponent(ctx, hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &enc); if (r != AMF_OK) { printf("init=create:%d\n", (int)r); return 3; }
    AMFRate fr = { 60, 1 }; AMFSize sz = { W, H }; AMFVariantStruct v;
    if (!hevc) {
        enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, I64(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY));
        enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, I64(8000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, I64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_RATE; v.rateValue = fr; enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, v);
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_SIZE; v.sizeValue = sz; enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, v);
    } else {
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, I64(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, I64(8000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, I64(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_RATE; v.rateValue = fr; enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, v);
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_SIZE; v.sizeValue = sz; enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, v);
    }
    r = enc->Init(AMF_SURFACE_BGRA, W, H); if (r != AMF_OK) { printf("init=encinit:%d\n", (int)r); return 3; }
    D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr; if (FAILED(dev->CreateTexture2D(&d, NULL, &tex))) { puts("init=tex"); return 3; }
    puts("init=0");
    std::vector<uint8_t> px((size_t)W * H * 4); FILE *out = fopen(argv[2], "wb");
    std::vector<double> lat; int got = 0, idrs = 0; double tsub[4096] = {0};
    for (int i = 0; i < N + 1; i++) {
        if (i < N) {
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                uint8_t *p = &px[((size_t)y * W + x) * 4]; uint8_t R, G, B;   /* BGRA memory: p[0]=B p[1]=G p[2]=R */
                bool right = x >= W / 2, bot = y >= H / 2;
                if (!right && !bot) { R = 255; G = 0; B = 0; } else if (right && !bot) { R = 0; G = 255; B = 0; }
                else if (!right && bot) { R = 0; G = 0; B = 255; } else { R = 255; G = 255; B = 255; }
                if (y >= 680 && x >= i * 10 && x < i * 10 + 40) { R = G = B = 0; }
                p[0] = B; p[1] = G; p[2] = R; p[3] = 255;
            }
            dc->UpdateSubresource(tex, 0, NULL, px.data(), W * 4, 0);
            AMFSurface *s = nullptr; r = ctx->CreateSurfaceFromDX11Native(tex, &s, NULL); if (r != AMF_OK) { printf("fail=surface:%d\n", (int)r); return 3; }
            s->SetPts(i * 166667); tsub[i] = now_ms();
            r = enc->SubmitInput(s); s->Release();
            if (r != AMF_OK && r != AMF_INPUT_FULL) { printf("fail=submit:%d frame %d\n", (int)r, i); return 3; }
        } else enc->Drain();
        for (;;) {
            AMFData *o = nullptr; r = enc->QueryOutput(&o); if (r != AMF_OK || !o) break;
            AMFBuffer *b = nullptr; o->QueryInterface(AMFBuffer::IID(), (void **)&b);
            AMFVariantStruct t; memset(&t, 0, sizeof t);
            o->GetProperty(hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &t);
            if (t.type == AMF_VARIANT_INT64 && t.int64Value == 0) idrs++;
            if (got < N) lat.push_back(now_ms() - tsub[std::min(got, N - 1)]);
            fwrite(b->GetNative(), 1, b->GetSize(), out); got++; b->Release(); o->Release();
        }
    }
    fclose(out);
    std::sort(lat.begin(), lat.end());
    if (!lat.empty()) printf("lat_ms=%.1f %.1f\n", lat[lat.size() / 2], lat.back());
    printf("RESULT frames=%d idr=%d\n", got, idrs);
    return got == N ? 0 : 1;
}
