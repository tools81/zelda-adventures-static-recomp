/* Direct3D 11 presenter: scales the 768x240 (NTSC) or 768x280 (PAL) CD-i frame to any window or
   monitor resolution (up to 4K and beyond) with selectable filters and
   aspect-ratio modes, including blurred side panels for 16:9 displays.

   The HD build (HD_BUILD) adds two upscalers, Cartoon HD and Cartoon HD+.
   Both work in two passes: a pre-pass filters the picture at the game's
   logical resolution (384 pixels per line), then an edge-directed upscale
   draws it at the output resolution:
     Cartoon HD   edge-preserving bilateral smoothing (removes dither noise,
                  keeps outlines), then interpolation along the local edge
                  direction (structure tensor) so contours come out smooth
                  and crisp instead of stair-stepped or blurred;
     Cartoon HD+  adds Kuwahara "cel" flattening of textured areas, dark ink
                  outlines on strong edges and a slight saturation boost. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include "runtime.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps, *ps_prep;
/* Cartoon filters: the picture filtered at logical resolution (pre-pass) */
static ID3D11Texture2D *lo_tex;
static ID3D11ShaderResourceView *lo_srv;
static ID3D11RenderTargetView *lo_rtv;
static int lo_w, lo_h;
static float aux_cb[4];
static ID3D11Buffer *cbuf;
static ID3D11SamplerState *samp_lin, *samp_point;
static int tex_w, tex_h, tex_mips;
static UINT buf_w, buf_h;
static HWND rwnd;

typedef struct {
    float rect[4];   /* destination rect in NDC: left, top, right, bottom */
    float src[4];    /* texture w, h, destination w, h (pixels) */
    float prm[4];    /* filter, mip level for backdrop, brightness, mode (0 image, 1 backdrop) */
    float uvx[4];    /* uv offset xy, uv scale zw */
    float aux[4];    /* Cartoon filters: logical size xy, pre-pass mode / HD+ flag z */
} CB;

static const char *shader_src =
"Texture2D tex : register(t0);\n"
"Texture2D lo : register(t1);\n"
"SamplerState sl : register(s0);\n"
"SamplerState sp : register(s1);\n"
"cbuffer C : register(b0) { float4 rect; float4 src; float4 prm; float4 uvx; float4 aux; };\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vsmain(uint id : SV_VertexID) {\n"
"  float2 c = float2(id & 1, id >> 1);\n"
"  VO o;\n"
"  o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0, 1);\n"
"  o.uv = uvx.xy + c * uvx.zw;\n"
"  return o;\n"
"}\n"
/* sharp bilinear: crisp pixels without shimmer at non-integer scales */
"float3 sharp(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 scale = src.zw / ts;\n"
"  float2 texel = uv * ts;\n"
"  float2 pre = max(floor(scale), 1.0);\n"
"  float2 fl = floor(texel);\n"
"  float2 cd = frac(texel) - 0.5;\n"
"  float2 rr = 0.5 - 0.5 / pre;\n"
"  float2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
"  return tex.SampleLevel(sl, (fl + f) / ts, 0).rgb;\n"
"}\n"
/* Catmull-Rom bicubic with 9 bilinear taps */
"float3 bicubic(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 sp2 = uv * ts;\n"
"  float2 t1 = floor(sp2 - 0.5) + 0.5;\n"
"  float2 f = sp2 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2;\n"
"  float2 o12 = w2 / w12;\n"
"  float2 t0 = (t1 - 1) / ts, t3 = (t1 + 2) / ts, t12 = (t1 + o12) / ts;\n"
"  float3 r = 0;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
"  return saturate(r);\n"
"}\n"
/* simple CRT: gaussian scanlines + aperture grille */
"float3 crt(float2 uv, float2 pos) {\n"
"  float ty = uv.y * src.y;\n"
"  float yc = floor(ty) + 0.5;\n"
"  float3 a = sharp(float2(uv.x, yc / src.y));\n"
"  float d = ty - yc;\n"
"  float lum = dot(a, float3(0.3, 0.6, 0.1));\n"
"  float width = lerp(6.0, 2.5, lum);\n"
"  float beam = exp(-d * d * width * 4.0);\n"
/* scanlines need several output pixels per source line to look right */
"  float strength = saturate((src.w / src.y - 2.5) / 4.0);\n"
"  float3 c = a * lerp(1.0, beam * 1.35, strength);\n"
"  int m = (int)pos.x % 3;\n"
"  float3 mask = m == 0 ? float3(1.0, 0.85, 0.85) : (m == 1 ? float3(0.85, 1.0, 0.85) : float3(0.85, 0.85, 1.0));\n"
"  mask = lerp(1.0, mask, strength);\n"
"  return saturate(c * mask * lerp(1.0, 1.1, strength));\n"
"}\n"
"float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
"float3 sharp_t(Texture2D t, float2 ts, float2 uv) {\n"
"  float2 scale = src.zw / ts;\n"
"  float2 texel = uv * ts;\n"
"  float2 pre = max(floor(scale), 1.0);\n"
"  float2 fl = floor(texel);\n"
"  float2 cd = frac(texel) - 0.5;\n"
"  float2 rr = 0.5 - 0.5 / pre;\n"
"  float2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
"  return t.SampleLevel(sl, (fl + f) / ts, 0).rgb;\n"
"}\n"
"float4 bicubic_t(Texture2D t, float2 ts, float2 uv) {\n"
"  float2 sp2 = uv * ts;\n"
"  float2 t1 = floor(sp2 - 0.5) + 0.5;\n"
"  float2 f = sp2 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2;\n"
"  float2 o12 = w2 / w12;\n"
"  float2 t0 = (t1 - 1) / ts, t3 = (t1 + 2) / ts, t12 = (t1 + o12) / ts;\n"
"  float4 r = 0;\n"
"  r += t.SampleLevel(sl, float2(t0.x, t0.y), 0) * w0.x * w0.y;\n"
"  r += t.SampleLevel(sl, float2(t12.x, t0.y), 0) * w12.x * w0.y;\n"
"  r += t.SampleLevel(sl, float2(t3.x, t0.y), 0) * w3.x * w0.y;\n"
"  r += t.SampleLevel(sl, float2(t0.x, t12.y), 0) * w0.x * w12.y;\n"
"  r += t.SampleLevel(sl, float2(t12.x, t12.y), 0) * w12.x * w12.y;\n"
"  r += t.SampleLevel(sl, float2(t3.x, t12.y), 0) * w3.x * w12.y;\n"
"  r += t.SampleLevel(sl, float2(t0.x, t3.y), 0) * w0.x * w3.y;\n"
"  r += t.SampleLevel(sl, float2(t12.x, t3.y), 0) * w12.x * w3.y;\n"
"  r += t.SampleLevel(sl, float2(t3.x, t3.y), 0) * w3.x * w3.y;\n"
"  return saturate(r);\n"
"}\n"
"/* one logical pixel (two horizontally doubled frame pixels) */\n"
"float3 srcpx(float2 q) {\n"
"  q = clamp(q, 0, aux.xy - 1);\n"
"  return tex.SampleLevel(sl, float2((2.0 * q.x + 1.0) / src.x, (q.y + 0.5) / src.y), 0).rgb;\n"
"}\n"
"/* 1 where the pixel shows plane A: the layer the game draws its characters on */\n"
"float front(float2 q) {\n"
"  q = clamp(q, 0, aux.xy - 1);\n"
"  return tex.SampleLevel(sp, float2((2.0 * q.x + 0.5) / src.x, (q.y + 0.5) / src.y), 0).a;\n"
"}\n"
"/* Pre-pass at logical resolution.  rgb: bilateral smoothing (Cartoon HD), or\n"
"   bilateral + Kuwahara cel flattening (Cartoon HD+).  a: ink-line strength\n"
"   (Cartoon HD+): the outline of the character layer. */\n"
"float4 psprep(VO i) : SV_Target {\n"
"  float2 q = floor(i.uv * aux.xy);\n"
"  float3 c0 = srcpx(q);\n"
"  float3 acc = 0; float ws = 0;\n"
"  [unroll] for (int y = -2; y <= 2; y++) [unroll] for (int x = -2; x <= 2; x++) {\n"
"    float3 c = srcpx(q + float2(x, y));\n"
"    float3 d = c - c0;\n"
"    float w = exp(-(x * x + y * y) / 4.5 - dot(d, d) / 0.03);\n"
"    acc += c * w; ws += w;\n"
"  }\n"
"  float3 bil = acc / ws;\n"
"  if (aux.z < 0.5) return float4(bil, 0);\n"
"  /* ink along the outline of the character layer (plane A), on its inside,\n"
"     so characters get outlines and the scenery does not */\n"
"  float f0 = front(q);\n"
"  float fmin = min(min(front(q + float2(1, 0)), front(q - float2(1, 0))),\n"
"                   min(front(q + float2(0, 1)), front(q - float2(0, 1))));\n"
"  float ink = f0 * (1.0 - fmin);\n"
"  float3 best = c0; float bv = 1e9;\n"
"  [unroll] for (int k = 0; k < 4; k++) {\n"
"    int ox = (k & 1) ? 2 : 0, oy = (k & 2) ? 2 : 0;\n"
"    float3 m = 0; float s1 = 0, s2 = 0;\n"
"    [unroll] for (int y2 = 0; y2 <= 2; y2++) [unroll] for (int x2 = 0; x2 <= 2; x2++) {\n"
"      float3 c = srcpx(q + float2(ox + x2 - 2, oy + y2 - 2));\n"
"      float l = luma(c); m += c; s1 += l; s2 += l * l;\n"
"    }\n"
"    float v = s2 / 9.0 - (s1 / 9.0) * (s1 / 9.0);\n"
"    if (v < bv) { bv = v; best = m / 9.0; }\n"
"  }\n"
"  return float4(lerp(bil, best, 0.4), ink);\n"
"}\n"
"/* edge-directed upscale of the pre-filtered picture */\n"
"float3 cartoon(float2 uv, bool plus) {\n"
"  float2 ts = aux.xy, px = 1.0 / ts;\n"
"  float jxx = 0, jxy = 0, jyy = 0;\n"
"  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {\n"
"    float2 p = (floor(uv * ts) + 0.5 + float2(x, y)) * px;\n"
"    float gx = luma(lo.SampleLevel(sp, p + float2(px.x, 0), 0).rgb) - luma(lo.SampleLevel(sp, p - float2(px.x, 0), 0).rgb);\n"
"    float gy = luma(lo.SampleLevel(sp, p + float2(0, px.y), 0).rgb) - luma(lo.SampleLevel(sp, p - float2(0, px.y), 0).rgb);\n"
"    float w = (x == 0 && y == 0) ? 4.0 : ((x == 0 || y == 0) ? 2.0 : 1.0);\n"
"    jxx += gx * gx * w; jxy += gx * gy * w; jyy += gy * gy * w;\n"
"  }\n"
"  jxx /= 16.0; jxy /= 16.0; jyy /= 16.0;\n"
"  float tr = jxx + jyy;\n"
"  float df = sqrt(max((jxx - jyy) * (jxx - jyy) * 0.25 + jxy * jxy, 0));\n"
"  float l1 = tr * 0.5 + df, l2 = tr * 0.5 - df;\n"
"  float2 n = abs(jxy) > 1e-7 ? normalize(float2(l1 - jyy, jxy)) : (jxx >= jyy ? float2(1, 0) : float2(0, 1));\n"
"  float2 t = float2(-n.y, n.x);\n"
"  float coh = (l1 - l2) / (l1 + l2 + 1e-6);\n"
"  /* follow only real edges: strong and with one clear direction */\n"
"  float edge = smoothstep(0.05, 0.15, sqrt(l1)) * smoothstep(0.55, 0.9, coh);\n"
"  float4 base = bicubic_t(lo, ts, uv);\n"
"  float3 along = 0; float ws = 0;\n"
"  [unroll] for (int k = -3; k <= 3; k++) {\n"
"    float s = k * 0.5; float w = exp(-s * s * 0.8);\n"
"    along += sharp_t(lo, ts, uv + t * s * px) * w; ws += w;\n"
"  }\n"
"  float3 c = lerp(base.rgb, along / ws, edge);\n"
"  if (plus) {\n"
"    /* ink lines: thin, from the pre-pass mask */\n"
"    float ink = smoothstep(0.25, 0.6, base.a);\n"
"    c *= 1.0 - 0.65 * ink;\n"
"    float yl = luma(c);\n"
"    c = lerp(float3(yl, yl, yl), c, 1.15);\n"
"  }\n"
"  return saturate(c);\n"
"}\n"
"float4 psmain(VO i) : SV_Target {\n"
"  if (prm.w > 0.5) {\n"
"    float2 st = exp2(prm.y) / src.xy;\n"
"    float3 b = 0; float wsum = 0;\n"
"    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
"      float w = exp(-(x * x + y * y) / 4.0);\n"
"      b += tex.SampleLevel(sl, i.uv + float2(x, y) * st, prm.y).rgb * w; wsum += w;\n"
"    }\n"
"    return float4(b / wsum * prm.z, 1);\n"
"  }\n"
"  int f = (int)prm.x;\n"
"  float3 c;\n"
"  if (f == 0) c = sharp(i.uv);\n"
"  else if (f == 1) c = bicubic(i.uv);\n"
"  else if (f == 2) c = crt(i.uv, i.pos.xy);\n"
"  else if (f == 4) c = cartoon(i.uv, false);\n"
"  else if (f == 5) c = cartoon(i.uv, true);\n"
"  else c = tex.SampleLevel(sp, i.uv, 0).rgb;\n"
"  return float4(c, 1);\n"
"}\n";

static const char *aspect_names[] = {"4:3 + blurred sides", "4:3", "stretch to fill", "zoom to fill width"};
static const char *filter_names[] = {"sharp", "smooth", "CRT", "nearest", "Cartoon HD", "Cartoon HD+"};
#ifdef HD_BUILD
RenderSettings render_settings = {0, 4};    /* the HD build starts with Cartoon HD */
#else
RenderSettings render_settings = {0, 0};
#endif

const char *render_aspect_name(int a) { return aspect_names[a % RENDER_ASPECTS]; }
const char *render_filter_name(int f) { return filter_names[f % RENDER_FILTERS]; }

static void release_rtv(void) {
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
}

static int make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    return FAILED(hr) ? -1 : 0;
}

int render_init(HWND hwnd) {
    rwnd = hwnd;
    RECT r;
    GetClientRect(hwnd, &r);
    buf_w = r.right > 0 ? (UINT)r.right : 1;
    buf_h = r.bottom > 0 ? (UINT)r.bottom : 1;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = buf_w;
    sd.BufferDesc.Height = buf_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                           &sd, &swap, &dev, NULL, &ctx);
    }
    if (FAILED(hr)) { LOG("Direct3D 11 unavailable (%08lx); using GDI", (unsigned long)hr); return -1; }
    /* we handle Alt+Enter ourselves (borderless fullscreen) */
    {
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fa)))
            IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (fa) IDXGIFactory_Release(fa);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
    }
    if (make_rtv()) return -1;

    ID3DBlob *vb = NULL, *pb = NULL, *pp = NULL, *err = NULL;
    size_t sl = strlen(shader_src);
    if (FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "vsmain", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "psmain", "ps_4_0", 0, 0, &pb, &err)) ||
        FAILED(D3DCompile(shader_src, sl, "present", NULL, NULL, "psprep", "ps_4_0", 0, 0, &pp, &err))) {
        LOG("shader compile failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return -1;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pp), ID3D10Blob_GetBufferSize(pp), NULL, &ps_prep);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    ID3D10Blob_Release(pp);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

    D3D11_SAMPLER_DESC s = {0};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_lin);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_point);
    LOG("renderer: Direct3D 11, %ux%u", buf_w, buf_h);
    return 0;
}

static int ensure_texture(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return 0;
    if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
    if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;                 /* full chain: low mips give the blurred backdrop */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) return -1;
    ID3D11Texture2D_GetDesc(tex, &td);
    tex_w = w; tex_h = h; tex_mips = (int)td.MipLevels;
    return 0;
}

/* render target for the Cartoon pre-pass, at logical resolution */
static int ensure_lo(int w, int h) {
    if (lo_tex && w == lo_w && h == lo_h) return 0;
    if (lo_srv) { ID3D11ShaderResourceView_Release(lo_srv); lo_srv = NULL; }
    if (lo_rtv) { ID3D11RenderTargetView_Release(lo_rtv); lo_rtv = NULL; }
    if (lo_tex) { ID3D11Texture2D_Release(lo_tex); lo_tex = NULL; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;     /* keeps the smooth gradients of the pre-pass */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &lo_tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)lo_tex, NULL, &lo_srv))) return -1;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)lo_tex, NULL, &lo_rtv))) return -1;
    lo_w = w; lo_h = h;
    return 0;
}

static void draw(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                 float mip, float bright, float dw, float dh) {
    CB c;
    c.rect[0] = l; c.rect[1] = t; c.rect[2] = r; c.rect[3] = b;
    c.src[0] = (float)tex_w; c.src[1] = (float)tex_h; c.src[2] = dw; c.src[3] = dh;
    c.prm[0] = (float)render_settings.filter; c.prm[1] = mip; c.prm[2] = bright; c.prm[3] = (float)backdrop;
    c.uvx[0] = u0; c.uvx[1] = v0; c.uvx[2] = us; c.uvx[3] = vs_;
    memcpy(c.aux, aux_cb, sizeof c.aux);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cbuf, 0, NULL, &c, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}

int render_present(const uint32_t *fb, int w, int h) {
    if (!dev) return -1;
    RECT cr;
    GetClientRect(rwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (IsIconic(rwnd)) return 0;
    if (cw != buf_w || ch != buf_h) {
        release_rtv();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return -1;
        buf_w = cw; buf_h = ch;
        if (make_rtv()) return -1;
    }
    if (ensure_texture(w, h)) return -1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, fb, (UINT)w * 4, 0);
    ID3D11DeviceContext_GenerateMips(ctx, srv);

    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11SamplerState *ss[2] = {samp_lin, samp_point};
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    ID3D11ShaderResourceView *srvs[2] = {srv, NULL};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);

    int cartoon = render_settings.filter >= 4;
    if (cartoon) {
        /* pre-pass: filter at logical resolution (two frame pixels per logical pixel) */
        if (ensure_lo(w / 2, h)) return -1;
        aux_cb[0] = (float)lo_w; aux_cb[1] = (float)lo_h;
        aux_cb[2] = render_settings.filter == 5 ? 1.0f : 0.0f; aux_cb[3] = 0;
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &lo_rtv, NULL);
        D3D11_VIEWPORT lvp = {0, 0, (float)lo_w, (float)lo_h, 0, 1};
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &lvp);
        ID3D11DeviceContext_PSSetShader(ctx, ps_prep, NULL, 0);
        draw(-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 1, (float)lo_w, (float)lo_h);
        srvs[1] = lo_srv;
    }

    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    D3D11_VIEWPORT vp = {0, 0, (float)cw, (float)ch, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);

    /* picture size in pixels for the chosen aspect mode */
    double win_aspect = (double)cw / ch, aspect = 4.0 / 3.0;
    double pw, ph;
    int mode = render_settings.aspect % RENDER_ASPECTS;
    if (mode == 2) { pw = cw; ph = ch; }
    else if (mode == 3) { pw = cw; ph = cw / aspect; if (ph < ch) { ph = ch; pw = ch * aspect; } }
    else if (win_aspect > aspect) { ph = ch; pw = ch * aspect; }
    else { pw = cw; ph = cw / aspect; }
    float l = (float)(-pw / cw), r = (float)(pw / cw), t = (float)(ph / ch), b = (float)(-ph / ch);

    if (mode == 0 && pw < cw - 1) {
        /* blurred, dimmed copy of the picture zoomed to cover the whole window */
        double cover_w = cw, cover_h = cw / aspect;
        if (cover_h < ch) { cover_h = ch; cover_w = ch * aspect; }
        float us = (float)(cw / cover_w), vs2 = (float)(ch / cover_h);
        float mip = (float)(tex_mips > 3 ? 3 : tex_mips - 1);
        draw(-1, 1, 1, -1, (1 - us) / 2, (1 - vs2) / 2, us, vs2, 1, mip, 0.5f, (float)cw, (float)ch);
    }
    draw(l, t, r, b, 0, 0, 1, 1, 0, 0, 1, (float)pw, (float)ph);
    if (cartoon) {      /* the pre-pass target is written again next frame */
        ID3D11ShaderResourceView *none[2] = {NULL, NULL};
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, none);
    }
    IDXGISwapChain_Present(swap, 0, 0);
    return 0;
}

/* debug (--capture): write the last presented picture to a BMP file */
int render_capture(const char *path) {
    if (!dev || !swap) return -1;
    ID3D11Texture2D *bb = NULL, *st = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    int rc = -1;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE *f = fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER bf = {0};
                BITMAPINFOHEADER bi = {0};
                bi.biSize = sizeof bi; bi.biWidth = (LONG)td.Width; bi.biHeight = -(LONG)td.Height;
                bi.biPlanes = 1; bi.biBitCount = 32;
                bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi;
                bf.bfSize = bf.bfOffBits + td.Width * td.Height * 4;
                fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f);
                for (UINT y = 0; y < td.Height; y++) fwrite((uint8_t *)m.pData + y * m.RowPitch, 4, td.Width, f);
                fclose(f);
                rc = 0;
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(bb);
    return rc;
}
