/* C4/NV12 test: BGRA -> NV12 rendering under DXVK, variants (1) NV12 + per-plane RTV, (2) separate R8 + R8G8
 * textures, (3) NV12 + per-plane UAV (compute), (4) R8/R8G8 UAV. Expected for RGB=(200,100,50):
 * Y~118 U~91 V~180 (BT.709 full range). Prints the values read back for each variant. */
#define COBJMACROS
#include <windows.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define W 1920
#define H 1080
static ID3D11Device *dev; static ID3D11DeviceContext *ctx; static ID3D11Device3 *dev3;
static const char *hlsl =
 "Texture2D t : register(t0);\n"
 "float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}\n"
 "float3 yuv(float3 c){float y=dot(c,float3(0.2126,0.7152,0.0722));return float3(y,(c.b-y)/1.8556+0.5,(c.r-y)/1.5748+0.5);}\n"
 "float psy(float4 p:SV_Position):SV_Target{return yuv(t.Load(int3(p.xy,0)).rgb).x;}\n"
 "float2 psuv(float4 p:SV_Position):SV_Target{return yuv(t.Load(int3(p.xy*2,0)).rgb).yz;}\n"
 "RWTexture2D<float> uy : register(u0);RWTexture2D<float2> uuv : register(u1);\n"
 "[numthreads(8,8,1)] void csy(uint3 i:SV_DispatchThreadID){uy[i.xy]=yuv(t.Load(int3(i.xy,0)).rgb).x;}\n"
 "[numthreads(8,8,1)] void csuv(uint3 i:SV_DispatchThreadID){uuv[i.xy]=yuv(t.Load(int3(i.xy*2,0)).rgb).yz;}\n";
static ID3DBlob *cs(const char *e, const char *p) {
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll"); typeof(&D3DCompile) comp = (void *)GetProcAddress(dc, "D3DCompile");
    ID3DBlob *b = NULL, *er = NULL;
    if (FAILED(comp(hlsl, strlen(hlsl), "s", NULL, NULL, e, p, 0, 0, &b, &er))) { printf("HLSL %s: %s\n", e, er ? (char *)ID3D10Blob_GetBufferPointer(er) : "?"); exit(2); }
    return b;
}
#define BP(b) ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b)
static ID3D11Texture2D *mk(DXGI_FORMAT f, UINT w, UINT h, UINT usage, UINT bind, UINT cpu, const void *data, UINT pitch, const char *what)
{
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, f, { 1, 0 }, usage, bind, cpu, 0 };
    D3D11_SUBRESOURCE_DATA sd = { data, pitch, 0 };
    ID3D11Texture2D *t = NULL; HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, data ? &sd : NULL, &t);
    if (FAILED(hr)) printf("  CreateTexture2D(%s) hr=0x%08lx\n", what, hr);
    return t;
}
static ID3D11ShaderResourceView *g_srv; static ID3D11VertexShader *vs; static ID3D11PixelShader *psy, *psuv;
static void draw(ID3D11RenderTargetView *rtv, ID3D11PixelShader *ps, UINT w, UINT h)
{
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0); ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11RenderTargetView *n = NULL; ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, &n, NULL);
}
static int rd(ID3D11Texture2D *src, DXGI_FORMAT f, UINT w, UINT h, int nbytes, const char *label, int uvoff)
{
    ID3D11Texture2D *s = mk(f, w, h, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, 0, "staging");
    if (!s) return -1;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) { puts("  Map failed"); return -1; }
    const uint8_t *p = m.pData; int x = w / 2, y = h / 2 & ~1;
    if (uvoff) { /* NV12: Y then UV at RowPitch*H */
        const uint8_t *uv = p + (size_t)m.RowPitch * h + (size_t)(y / 2) * m.RowPitch + (x & ~1);
        printf("  %s : Y=%u U=%u V=%u (pitch %u)\n", label, p[(size_t)y * m.RowPitch + x], uv[0], uv[1], m.RowPitch);
    } else if (nbytes == 1) printf("  %s : plane=%u\n", label, p[(size_t)y * m.RowPitch + x]);
    else printf("  %s : plane=(%u,%u)\n", label, p[(size_t)y * m.RowPitch + x * 2], p[(size_t)y * m.RowPitch + x * 2 + 1]);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0); ID3D11Texture2D_Release(s);
    return 0;
}
int main(void)
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice 0x%08lx\n", hr); return 2; }
    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, (void **)&dev3);
    printf("fl=0x%x Device3 hr=0x%08lx\n", fl, hr);
    UINT sup; for (int f = 0; f < 3; f++) { DXGI_FORMAT ff[] = { DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM };
        ID3D11Device_CheckFormatSupport(dev, ff[f], &sup); printf("support fmt %d = 0x%x (RT=%d UAV=%d)\n", ff[f], sup, !!(sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET), !!(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)); }
    static uint32_t px[W * H]; for (int i = 0; i < W * H; i++) px[i] = 0xff000000u | (200u << 16) | (100u << 8) | 50u; /* BGRA: R=200 G=100 B=50 */
    ID3D11Texture2D *src = mk(DXGI_FORMAT_B8G8R8A8_UNORM, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, px, W * 4, "src");
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)src, NULL, &g_srv);
    ID3DBlob *bv = cs("vs", "vs_4_0"), *by = cs("psy", "ps_4_0"), *buv = cs("psuv", "ps_4_0");
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &vs); ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(by), ID3D10Blob_GetBufferSize(by), NULL, &psy); ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(buv), ID3D10Blob_GetBufferSize(buv), NULL, &psuv);

    puts("V1 : NV12 DEFAULT RENDER_TARGET, RTV R8 (plane 0) + RTV R8G8 (plane 1) [CreateRenderTargetView1]");
    ID3D11Texture2D *nv = mk(DXGI_FORMAT_NV12, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL, 0, "NV12");
    if (nv && dev3) {
        ID3D11RenderTargetView1 *ry = NULL, *ruv = NULL;
        D3D11_RENDER_TARGET_VIEW_DESC1 d = { DXGI_FORMAT_R8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D, { .Texture2D = { 0, 0 } } };
        hr = ID3D11Device3_CreateRenderTargetView1(dev3, (ID3D11Resource *)nv, &d, &ry); printf("  RTV Y hr=0x%08lx\n", hr);
        d.Format = DXGI_FORMAT_R8G8_UNORM; d.Texture2D.PlaneSlice = 1;
        hr = ID3D11Device3_CreateRenderTargetView1(dev3, (ID3D11Resource *)nv, &d, &ruv); printf("  RTV UV hr=0x%08lx\n", hr);
        if (ry && ruv) { draw((ID3D11RenderTargetView *)ry, psy, W, H); draw((ID3D11RenderTargetView *)ruv, psuv, W / 2, H / 2); rd(nv, DXGI_FORMAT_NV12, W, H, 0, "NV12 read back", 1); }
    }
    puts("V2 : separate R8 + R8G8 RENDER_TARGET textures");
    ID3D11Texture2D *ty = mk(DXGI_FORMAT_R8_UNORM, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, 0, "R8");
    ID3D11Texture2D *tuv = mk(DXGI_FORMAT_R8G8_UNORM, W / 2, H / 2, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, 0, "R8G8");
    if (ty && tuv) {
        ID3D11RenderTargetView *a = NULL, *b = NULL;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)ty, NULL, &a); ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tuv, NULL, &b);
        draw(a, psy, W, H); draw(b, psuv, W / 2, H / 2);
        rd(ty, DXGI_FORMAT_R8_UNORM, W, H, 1, "R8 read back", 0); rd(tuv, DXGI_FORMAT_R8G8_UNORM, W / 2, H / 2, 2, "R8G8 read back", 0);
        puts("V2b : CopySubresourceRegion R8/R8G8 -> NV12 planes (CopyResource between compatible formats)");
        if (nv) { /* planes via subresource 0: not separately addressable in D3D11 without views; direct attempt */
            D3D11_BOX bx = { 0, 0, 0, W, H, 1 };
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)nv, 0, 0, 0, 0, (ID3D11Resource *)ty, 0, &bx);
            rd(nv, DXGI_FORMAT_NV12, W, H, 0, "NV12 after Y copy", 1);
        }
    }
    puts("V3 : NV12 + UAV plane 0 (R8) / plane 1 (R8G8), compute");
    ID3D11Texture2D *nvu = mk(DXGI_FORMAT_NV12, W, H, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, NULL, 0, "NV12 UAV");
    if (nvu && dev3) {
        ID3D11UnorderedAccessView1 *uy = NULL, *uuv = NULL;
        D3D11_UNORDERED_ACCESS_VIEW_DESC1 d = { DXGI_FORMAT_R8_UNORM, D3D11_UAV_DIMENSION_TEXTURE2D, { .Texture2D = { 0, 0 } } };
        hr = ID3D11Device3_CreateUnorderedAccessView1(dev3, (ID3D11Resource *)nvu, &d, &uy); printf("  UAV Y hr=0x%08lx\n", hr);
        d.Format = DXGI_FORMAT_R8G8_UNORM; d.Texture2D.PlaneSlice = 1;
        hr = ID3D11Device3_CreateUnorderedAccessView1(dev3, (ID3D11Resource *)nvu, &d, &uuv); printf("  UAV UV hr=0x%08lx\n", hr);
        if (uy && uuv) {
            ID3DBlob *c1 = cs("csy", "cs_5_0"), *c2 = cs("csuv", "cs_5_0"); ID3D11ComputeShader *s1, *s2;
            ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(c1), ID3D10Blob_GetBufferSize(c1), NULL, &s1); ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(c2), ID3D10Blob_GetBufferSize(c2), NULL, &s2);
            ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &g_srv);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, (ID3D11UnorderedAccessView **)&uy, NULL);
            ID3D11DeviceContext_CSSetShader(ctx, s1, NULL, 0); ID3D11DeviceContext_Dispatch(ctx, W / 8, H / 8, 1);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 1, 1, (ID3D11UnorderedAccessView **)&uuv, NULL);
            ID3D11DeviceContext_CSSetShader(ctx, s2, NULL, 0); ID3D11DeviceContext_Dispatch(ctx, W / 16, H / 16, 1);
            rd(nvu, DXGI_FORMAT_NV12, W, H, 0, "NV12 read back", 1);
        }
    }
    puts("Expected: Y~118 U~91 V~180");
    return 0;
}
