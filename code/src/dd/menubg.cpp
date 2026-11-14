#include "menubg.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../config.h"
#include "../patch.h"
#include "texreplace.h"

namespace dd {

namespace {

// Full-window pass. Inside the game's 640x480 area: the frame, bilinear like
// the normal present, but taps that are key green are left out and what is
// left is laid over (picture over flag). Outside it: the flag. Not the main
// menu (no key green in the corners): the plain frame and black bars.
const char kMenuShader[] = R"(
cbuffer Params : register(b0) {
  float4 rect;  // game area in window pixels: x0 y0 w h
  float4 info;  // x: seconds, y: picture loaded
};
Texture2D frame : register(t0);
Texture2D art : register(t1);
SamplerState lin : register(s0);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 c = float2(id & 1, id >> 1);
  return float4(c.x * 2 - 1, 1 - c.y * 2, 0, 1);
}

bool IsKey(float3 c) { return c.g > 0.85 && c.r < 0.12 && c.b < 0.12; }

// The cloth as a height field (in flag heights; u = menu pixels / 480) with
// its slopes. Soft bulges with sharp creases between them, fanning out
// diagonally from the top of the pole on the left and growing towards the
// free end, a broad slow swell under them, slow drift and gusts.
void Cloth(float2 u, float t, out float h, out float hx, out float hy) {
  float grow = 0.30 + 0.70 * saturate(u.x / 1.333);
  float gust = 0.85 + 0.15 * sin(t * 0.19) + 0.05 * sin(t * 0.47);
  float q1 = u.y * 1.6 + u.x * 1.2 + t * 0.23;
  float th1 = u.x * 9.0 - t * 1.0 + u.y * 2.2 + 0.8 * sin(q1);
  float q2 = u.y * 2.4 - t * 0.31;
  float th2 = u.x * 5.0 - t * 0.65 - u.y * 1.4 + 1.9 + 0.5 * sin(q2);
  float th3 = u.x * 14.0 - t * 1.5 + u.y * 4.5 + 0.7;
  float a1 = 0.050 * grow * gust * (0.75 + 0.25 * sin(u.y * 2.0 + u.x * 1.5 - t * 0.4));
  float a2 = 0.070 * grow * gust;
  float a3 = 0.012 * grow;
  float s1 = sin(th1);
  float r1 = sqrt(s1 * s1 + 0.02);
  float c1 = r1 - 0.65;              // crease shape: |sin| with a rounded floor
  float d1 = s1 * cos(th1) / r1;     // its derivative
  h = a1 * c1 + a2 * sin(th2) + a3 * sin(th3);
  float dq1 = 0.8 * cos(q1);
  hx = a1 * d1 * (9.0 + dq1 * 1.2) + a2 * cos(th2) * 5.0 + a3 * cos(th3) * 14.0;
  hy = a1 * d1 * (2.2 + dq1 * 1.6) + a2 * cos(th2) * (-1.4 + 0.5 * cos(q2) * 2.4) + a3 * cos(th3) * 4.5;
}

// Dark blue chequered flag, lit as cloth. p: menu pixels (0..640, 0..480).
float3 Flag(float2 p, float t) {
  float2 u = p / 480.0;
  float h, hx, hy;
  Cloth(u, t, h, hx, hy);
  // Seen a little from the side and above: the pattern bends into the folds
  // and bunches up where the cloth turns away.
  float2 f = float2(u.x - 0.45 * h - 0.2 * h * hx, u.y + 0.9 * h) * 7.5;  // 64 px squares
  float3 n = normalize(float3(-hx, -hy, 1));
  float3 L = normalize(float3(-0.70, -0.30, 0.65));  // low light from the upper left
  float3 H = normalize(L + float3(0, 0, 1));
  float diff = saturate(dot(n, L));
  float spec = pow(saturate(dot(n, H)), 16.0) * 0.30;  // satin sheen on the bulges
  float ao = clamp(0.80 + h * 3.0, 0.40, 1.0);          // creases darker
  float2 tri = abs(frac(f * 0.5) * 2.0 - 1.0) * 2.0 - 1.0;  // square wave, also left of the menu
  float w = 2.0 * 7.5 * 640.0 / (max(rect.z, 1.0) * 480.0);  // one window pixel of edge softening
  float chk = 0.5 + 0.5 * clamp(tri.x / w, -1, 1) * clamp(tri.y / w, -1, 1);
  float3 dark = float3(0.055, 0.050, 0.130);
  float3 light = float3(0.175, 0.150, 0.330);
  float3 c = lerp(dark, light, chk);
  // The original's vertical light: darkest at the top and bottom, a band
  // of light a little above the middle.
  float band = 0.30 + 0.95 * exp(-pow((p.y / 480.0 - 0.47) / 0.30, 2.0));
  float shade = (0.15 + 1.45 * pow(diff, 1.3)) * ao;
  return c * band * shade * 1.2 + spec * band * ao * float3(0.5, 0.5, 0.75);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target {
  float2 g = (pos.xy - rect.xy) / rect.zw * float2(640, 480);
  float3 under = Flag(g, info.x);
  bool inside = all(g >= 0) && all(g < float2(640, 480));
  bool menu = IsKey(frame.Load(int3(3, 3, 0)).rgb) || IsKey(frame.Load(int3(636, 3, 0)).rgb) ||
              IsKey(frame.Load(int3(3, 476, 0)).rgb) || IsKey(frame.Load(int3(636, 476, 0)).rgb);
  float2 uv = g / float2(640, 480);
  if (!menu) {
    if (!inside) return float4(0, 0, 0, 1);
    return float4(frame.SampleLevel(lin, uv, 0).rgb, 1);
  }
  if (!inside) return float4(under, 1);
  if (info.y != 0) {
    float4 a = art.SampleLevel(lin, uv, 0);  // premultiplied
    under = under * (1 - a.a) + a.rgb;
  }
  float2 q = g - 0.5;
  float2 i0 = floor(q);
  float2 fr = q - i0;
  float4 acc = 0;
  [unroll] for (int k = 0; k < 4; ++k) {
    int2 o = int2(k & 1, k >> 1);
    int2 ii = clamp(int2(i0) + o, int2(0, 0), int2(639, 479));
    float3 c = frame.Load(int3(ii, 0)).rgb;
    float wx = o.x != 0 ? fr.x : 1 - fr.x;
    float wy = o.y != 0 ? fr.y : 1 - fr.y;
    float wt = wx * wy;
    if (!IsKey(c)) acc += float4(c * wt, wt);
  }
  return float4(under * (1 - acc.a) + acc.rgb, 1);
}
)";

struct MenuCB {
  float rect[4];
  float info[4];
};

bool g_checked = false, g_wanted = false;
std::vector<uint8_t> g_art;  // premultiplied BGRA 640x480
bool g_failed = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11Texture2D* g_art_tex = nullptr;
ID3D11ShaderResourceView* g_art_srv = nullptr;
LARGE_INTEGER g_t0{}, g_freq{};
unsigned g_frames = 0;

std::wstring ArtPath() {
  const char* rel = GetConfig().menu_background;
  wchar_t wrel[MAX_PATH] = {};
  MultiByteToWideChar(CP_ACP, 0, rel, -1, wrel, MAX_PATH);
  if (wrel[0] && (wrel[1] == L':' || (wrel[0] == L'\\' && wrel[1] == L'\\'))) return wrel;
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  const size_t slash = s.find_last_of(L'\\');
  return (slash == std::wstring::npos ? std::wstring(L".") : s.substr(0, slash)) + L"\\" + wrel;
}

bool CreateResources() {
  Backend& b = B();
  ID3DBlob* vs = Compile(kMenuShader, "vs_main", "vs_4_0");
  ID3DBlob* ps = Compile(kMenuShader, "ps_main", "ps_4_0");
  if (vs) b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
  if (ps) b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps);
  SafeRelease(vs);
  SafeRelease(ps);
  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(MenuCB);
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  b.device->CreateBuffer(&cb, nullptr, &g_cb);
  if (!g_art.empty()) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 640;
    td.Height = 480;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{g_art.data(), 640 * 4, 0};
    if (SUCCEEDED(b.device->CreateTexture2D(&td, &sd, &g_art_tex)))
      b.device->CreateShaderResourceView(g_art_tex, nullptr, &g_art_srv);
  }
  QueryPerformanceFrequency(&g_freq);
  QueryPerformanceCounter(&g_t0);
  const bool ok = g_vs && g_ps && g_cb;
  patch::Log("menu flag: GPU resources %s (shader %s, picture %s)", ok ? "ready" : "FAILED",
             g_vs && g_ps ? "ok" : "failed", g_art_srv ? "ok" : "none");
  return ok;
}

}  // namespace

bool MenuBgWanted() {
  if (g_checked) return g_wanted;
  g_checked = true;
  const Config& c = GetConfig();
  if (!c.menu_flag) {
    patch::Log("menu flag: MenuFlag=0");
    return false;
  }
  if (!Ready() || IsD3D9()) {
    patch::Log("menu flag: needs the Direct3D 11 renderer (%s), main menu left as it was",
               !Ready() ? "no device yet" : "running Direct3D 9");
    return false;
  }
  const std::wstring path = ArtPath();
  std::vector<uint8_t> px;
  UINT w = 0, h = 0;
  if (!DecodeImageFile(path, px, w, h)) {
    patch::Log("menu flag: no picture at %ls (MenuBackground), main menu left as it was", path.c_str());
    return false;
  }
  if (w != 640 || h != 480) {
    patch::Log("menu flag: %ls is %ux%u, needs 640x480; main menu left as it was", path.c_str(), w, h);
    return false;
  }
  unsigned clear = 0, solid = 0;
  for (size_t i = 0; i < px.size(); i += 4) {
    const unsigned a = px[i + 3];
    clear += a == 0;
    solid += a == 255;
    for (int k = 0; k < 3; ++k) px[i + k] = static_cast<uint8_t>((px[i + k] * a + 127) / 255);
  }
  g_art.swap(px);
  g_wanted = true;
  patch::Log("menu flag: on, picture %ls (%u transparent, %u solid, %u partly transparent pixels)", path.c_str(), clear,
             solid, 640u * 480u - clear - solid);
  return true;
}

bool MenuBgPresent(ID3D11ShaderResourceView* frame, UINT frame_w, UINT frame_h, const RECT& r) {
  if (!g_wanted || g_failed || !frame || frame_w != 640 || frame_h != 480 || IsD3D9()) return false;
  Backend& b = B();
  if (!g_vs && !CreateResources()) {
    g_failed = true;
    return false;
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  MenuCB cb{};
  cb.rect[0] = static_cast<float>(r.left);
  cb.rect[1] = static_cast<float>(r.top);
  cb.rect[2] = static_cast<float>(r.right - r.left);
  cb.rect[3] = static_cast<float>(r.bottom - r.top);
  cb.info[0] = static_cast<float>(std::fmod(static_cast<double>(now.QuadPart - g_t0.QuadPart) / g_freq.QuadPart, 3600.0));
  cb.info[1] = g_art_srv ? 1.0f : 0.0f;
  ContextTouched();
  b.ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
  b.ctx->OMSetRenderTargets(1, &b.swap_rtv, nullptr);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(b.swap_w), static_cast<float>(b.swap_h), 0, 1};
  b.ctx->RSSetViewports(1, &vp);
  b.ctx->RSSetState(b.blit_raster);
  b.ctx->OMSetDepthStencilState(b.no_depth, 0);
  b.ctx->OMSetBlendState(b.no_blend, nullptr, 0xffffffff);
  b.ctx->IASetInputLayout(nullptr);
  b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  b.ctx->VSSetShader(g_vs, nullptr, 0);
  b.ctx->PSSetShader(g_ps, nullptr, 0);
  b.ctx->PSSetConstantBuffers(0, 1, &g_cb);
  ID3D11ShaderResourceView* srv[2] = {frame, g_art_srv};
  b.ctx->PSSetShaderResources(0, 2, srv);
  b.ctx->PSSetSamplers(0, 1, &b.linear_sampler);
  b.ctx->Draw(4, 0);
  ID3D11ShaderResourceView* none[2] = {};
  b.ctx->PSSetShaderResources(0, 2, none);
  if (++g_frames == 1 || g_frames % 3600 == 0)
    patch::Log("menu flag: %u menu frames drawn through the flag pass (window %ux%u, game area %ld,%ld %ldx%ld)",
               g_frames, b.swap_w, b.swap_h, r.left, r.top, r.right - r.left, r.bottom - r.top);
  return true;
}

}  // namespace dd
