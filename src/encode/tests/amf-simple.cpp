/* amf-simple.exe — test AMF client following the SDK's SimpleEncoder pattern:
 * LoadLibrary(amfrt64.dll) -> AMFInit -> CreateContext -> InitDX11 -> CreateComponent -> Init ->
 * SubmitInput(D3D11 texture) / QueryOutput. Usage: amf-simple.exe <avc|hevc> <nv12|bgra> <output> [frames]
 * IDR forced at frame 30 via ForcePictureType on the surface; bitrate changed on the fly at frame 45. */
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
int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    bool hevc = !strcmp(argv[1], "hevc"), nv12 = !strcmp(argv[2], "nv12");
    int N = argc > 4 ? atoi(argv[4]) : 90, W = 1280, H = 720;
    HMODULE m = LoadLibraryW(L"amfrt64.dll"); if (!m) { puts("LoadLibrary amfrt64.dll failed"); return 1; }
    auto qv = (AMFQueryVersion_Fn)GetProcAddress(m, "AMFQueryVersion"); auto in = (AMFInit_Fn)GetProcAddress(m, "AMFInit");
    amf_uint64 ver = 0; if (!qv || !in || qv(&ver) != AMF_OK) { puts("AMFQueryVersion failed"); return 1; }
    printf("version %llx\n", (unsigned long long)ver);
    AMFFactory *f = nullptr; if (in(ver, &f) != AMF_OK) { puts("AMFInit failed"); return 1; }
    AMFContext *ctx = nullptr; f->CreateContext(&ctx);
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *dc = nullptr; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &dc);
    if (FAILED(hr)) { printf("D3D11CreateDevice 0x%lx\n", (unsigned long)hr); return 1; }
    AMF_RESULT r = ctx->InitDX11(dev); if (r != AMF_OK) { printf("InitDX11 -> %d\n", (int)r); return 1; }
    AMFComponent *enc = nullptr;
    r = f->CreateComponent(ctx, hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC, &enc); if (r != AMF_OK) { printf("CreateComponent %d\n", (int)r); return 1; }
    AMFRate fr = { 60, 1 }; AMFSize sz = { W, H };
    AMFVariantStruct v;
    if (!hevc) {
        enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, I64(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY));
        enc->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, I64(4000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, I64(6000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, I64(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        enc->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, I64(300));
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_RATE; v.rateValue = fr; enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, v);
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_SIZE; v.sizeValue = sz; enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, v);
    } else {
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, I64(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, I64(4000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, I64(6000000));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, I64(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
        enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, I64(300));
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_RATE; v.rateValue = fr; enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, v);
        memset(&v, 0, sizeof v); v.type = AMF_VARIANT_SIZE; v.sizeValue = sz; enc->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, v);
    }
    r = enc->Init(nv12 ? AMF_SURFACE_NV12 : AMF_SURFACE_BGRA, W, H); if (r != AMF_OK) { printf("Init -> %d\n", (int)r); return 1; }

    D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr; hr = dev->CreateTexture2D(&d, NULL, &tex);
    if (FAILED(hr)) { printf("CreateTexture2D 0x%lx\n", (unsigned long)hr); return 1; }
    std::vector<uint8_t> px((size_t)W * H * 3 / 2 * (nv12 ? 1 : 3));   /* enough for NV12 (1.5 B/px) and BGRA (4 B/px) */
    FILE *out = fopen(argv[3], "wb"); int got = 0, idrs = 0;
    for (int i = 0; i < N + 1; i++) {
        if (i < N) {
            if (nv12) {
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[(size_t)y * W + x] = (uint8_t)(((x + i * 4) / 8 + y / 8) & 1 ? 200 : 60);
                memset(&px[(size_t)W * H], 128, (size_t)W * H / 2);
                dc->UpdateSubresource(tex, 0, NULL, px.data(), W, 0);
            } else {
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *p = &px[((size_t)y * W + x) * 4]; uint8_t c = (((x + i * 4) / 8 + y / 8) & 1) ? 220 : 40; p[0] = c; p[1] = (uint8_t)(x * 255 / W); p[2] = (uint8_t)(y * 255 / H); p[3] = 255; }
                dc->UpdateSubresource(tex, 0, NULL, px.data(), W * 4, 0);
            }
            AMFSurface *s = nullptr; r = ctx->CreateSurfaceFromDX11Native(tex, &s, NULL); if (r != AMF_OK) { printf("CreateSurfaceFromDX11Native %d\n", (int)r); return 1; }
            s->SetPts(i * 166667);
            if (i == 30) s->SetProperty(hevc ? AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE : AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, I64(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            if (i == 45) enc->SetProperty(hevc ? AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE : AMF_VIDEO_ENCODER_TARGET_BITRATE, I64(2000000));
            r = enc->SubmitInput(s); s->Release();
            if (r != AMF_OK) { printf("SubmitInput %d frame %d\n", (int)r, i); return 1; }
        } else enc->Drain();
        for (;;) {
            AMFData *o = nullptr; r = enc->QueryOutput(&o);
            if (r != AMF_OK || !o) break;
            AMFBuffer *b = nullptr; o->QueryInterface(AMFBuffer::IID(), (void **)&b);
            AMFVariantStruct t; memset(&t, 0, sizeof t);
            o->GetProperty(hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &t);
            if (t.type == AMF_VARIANT_INT64 && t.int64Value == 0) { idrs++; printf("IDR in output #%d\n", got); }
            fwrite(b->GetNative(), 1, b->GetSize(), out); got++;
            b->Release(); o->Release();
        }
    }
    fclose(out);
    printf("RESULT frames=%d idr=%d\n", got, idrs);
    enc->Terminate(); enc->Release(); ctx->Terminate(); ctx->Release();
    return got == N ? 0 : 1;
}
