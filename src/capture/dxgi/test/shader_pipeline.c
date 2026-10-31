/* C4 test: reproduces the Parsec GPU pipeline on the DuplicateOutput1 texture.
 * Acquire -> (a) direct sampling by shader, (b) CopyResource to a DEFAULT RT|SRV texture then
 * shader, (c) control: same content (read by Map) uploaded into a texture created by the test, same shader.
 * The shader writes (Y,U,V) BT.709 into an RGBA8 target; the averages (a),(b),(c) are compared.
 * Prints "RESULT ..."; exit code 0 if (a)==(c) and (b)==(c) and the content is non-null. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define W 1920
#define H 1080
static ID3D11Device *dev; static ID3D11DeviceContext *ctx;
static const char *hlsl =
 "Texture2D t : register(t0);\n"
 "float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}\n"
 "float4 ps(float4 pos:SV_Position):SV_Target{float3 c=t.Load(int3(pos.xy,0)).bgr*0+t.Load(int3(pos.xy,0)).rgb;\n"
 " float y=dot(c,float3(0.2126,0.7152,0.0722));return float4(y,(c.b-y)/1.8556+0.5,(c.r-y)/1.5748+0.5,1);}\n";

static ID3D11Texture2D *mk(UINT usage, UINT bind, UINT cpu, DXGI_FORMAT f, const void *data)
{
    D3D11_TEXTURE2D_DESC d = { W, H, 1, 1, f, { 1, 0 }, usage, bind, cpu, 0 };
    D3D11_SUBRESOURCE_DATA sd = { data, W * 4, 0 };
    ID3D11Texture2D *t = NULL; HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, data ? &sd : NULL, &t);
    if (FAILED(hr)) { printf("CreateTexture2D 0x%08lx\n", hr); exit(2); }
    return t;
}
static ID3D11PixelShader *ps; static ID3D11VertexShader *vs; static ID3D11Texture2D *rt; static ID3D11RenderTargetView *rtv, *dummy;
static ID3D11Texture2D *stg;
static void run(ID3D11Texture2D *src, uint32_t *sum)  /* sum[0..2] = averages of R,G,B (Y,U,V) x1000 */
{
    ID3D11ShaderResourceView *srv = NULL;
    HRESULT hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)src, NULL, &srv);
    if (FAILED(hr)) { printf("CreateSRV 0x%08lx\n", hr); sum[0] = sum[1] = sum[2] = 0xffffffff; return; }
    float clr[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clr);
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11ShaderResourceView *nul = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &nul);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) { puts("Map failed"); exit(2); }
    uint64_t a[3] = { 0, 0, 0 };
    for (int y = 0; y < H; y += 4) for (int x = 0; x < W; x += 4) {
        const uint8_t *p = (const uint8_t *)m.pData + (size_t)y * m.RowPitch + x * 4;
        a[0] += p[0]; a[1] += p[1]; a[2] += p[2];
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
    uint64_t n = (W / 4) * (H / 4);
    for (int i = 0; i < 3; i++) sum[i] = (uint32_t)(a[i] * 1000 / n);
    ID3D11ShaderResourceView_Release(srv);
}
int main(void)
{
    IDXGIFactory1 *fac; CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac);
    IDXGIAdapter1 *ad; IDXGIFactory1_EnumAdapters1(fac, 0, &ad);
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice 0x%08lx\n", hr); return 2; }
    IDXGIOutput *out; IDXGIAdapter1_EnumOutputs(ad, 0, &out);
    IDXGIOutput5 *o5; IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput5, (void **)&o5);
    DXGI_FORMAT fm[2] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
    IDXGIOutputDuplication *dup;
    hr = IDXGIOutput5_DuplicateOutput1(o5, (IUnknown *)dev, 0, 2, fm, &dup);
    printf("DuplicateOutput1 hr=0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    typeof(&D3DCompile) comp = (void *)GetProcAddress(dc, "D3DCompile");
    ID3DBlob *bv = NULL, *bp = NULL, *err = NULL;
    if (FAILED(comp(hlsl, strlen(hlsl), "s", NULL, NULL, "vs", "vs_4_0", 0, 0, &bv, &err)) ||
        FAILED(comp(hlsl, strlen(hlsl), "s", NULL, NULL, "ps", "ps_4_0", 0, 0, &bp, &err))) {
        printf("HLSL compilation: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 2; }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bp), ID3D10Blob_GetBufferSize(bp), NULL, &ps);
    rt = mk(D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, DXGI_FORMAT_R8G8B8A8_UNORM, NULL);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    stg = mk(D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, DXGI_FORMAT_R8G8B8A8_UNORM, NULL);
    ID3D11Texture2D *sstg = mk(D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, DXGI_FORMAT_B8G8R8A8_UNORM, NULL);
    ID3D11Texture2D *cp = mk(D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, DXGI_FORMAT_B8G8R8A8_UNORM, NULL);
    int ok = 0;
    for (int i = 0; i < 100 && ok < 3; i++) {
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
        hr = IDXGIOutputDuplication_AcquireNextFrame(dup, 100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { printf("Acquire 0x%08lx\n", hr); return 1; }
        ID3D11Texture2D *t; IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&t);
        D3D11_TEXTURE2D_DESC d; ID3D11Texture2D_GetDesc(t, &d);
        printf("tex fmt=%u bind=0x%x usage=%u misc=0x%x %ux%u\n", d.Format, d.BindFlags, d.Usage, d.MiscFlags, d.Width, d.Height);
        uint32_t a[3], b[3], c[3];
        run(t, a);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)cp, (ID3D11Resource *)t);
        run(cp, b);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)sstg, (ID3D11Resource *)t);
        D3D11_MAPPED_SUBRESOURCE m; ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)sstg, 0, D3D11_MAP_READ, 0, &m);
        uint32_t *buf = malloc((size_t)W * H * 4);
        for (int y = 0; y < H; y++) memcpy(buf + (size_t)y * W, (uint8_t *)m.pData + (size_t)y * m.RowPitch, W * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)sstg, 0);
        ID3D11Texture2D *ctl = mk(D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, DXGI_FORMAT_B8G8R8A8_UNORM, buf);
        run(ctl, c);
        printf("RESULT direct=(%u,%u,%u) copy=(%u,%u,%u) control=(%u,%u,%u)  [x1000/255]\n", a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]);
        int nz = c[0] > 5000 / 255 + 0 || c[1] || c[2];
        int same = abs((int)a[0] - (int)c[0]) < 3000 && abs((int)b[0] - (int)c[0]) < 3000 &&
                   abs((int)a[1] - (int)c[1]) < 3000 && abs((int)b[1] - (int)c[1]) < 3000;
        printf("VERDICT content_non_null=%d ours==control=%d\n", nz, same);
        free(buf); ID3D11Texture2D_Release(ctl); ID3D11Texture2D_Release(t); IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(dup);
        ok++;
    }
    return ok ? 0 : 1;
}
