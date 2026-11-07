#include "backend.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "../patch.h"

namespace dd {

namespace {
Backend g_backend;
bool g_device_failed = false;
RECT g_monitor_rect{};

const char kBlitShader[] = R"(
cbuffer Params : register(b0) {
  float4 dst;    // x0 y0 x1 y1 in NDC
  float4 src;    // u0 v0 u1 v1
  float4 fill;
  float4 flags;  // x: use texture, y: colour key
  float4 key;    // colour key (rgb)
};
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
  float2 c = float2(id & 1, id >> 1);
  VSOut o;
  o.pos = float4(lerp(dst.x, dst.z, c.x), lerp(dst.y, dst.w, c.y), 0, 1);
  o.uv = float2(lerp(src.x, src.z, c.x), lerp(src.y, src.w, c.y));
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  if (flags.x == 0) return fill;
  float4 c = tex.Sample(smp, i.uv);
  if (flags.y != 0 && all(abs(c.rgb - key.rgb) < 0.002)) discard;
  return flags.z != 0 ? float4(c.rgb, 1) : c;
}
)";

struct BlitCB {
  float dst[4];
  float src[4];
  float fill[4];
  float flags[4];
  float key[4];
};

}  // namespace

std::recursive_mutex& Lock() {
  static std::recursive_mutex m;
  return m;
}

void Log(const char* fmt, ...) {
  char buf[480];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  patch::Log("d3d11: %s", buf);
}

void LogOnce(const char* fmt, ...) {
  static std::set<std::string> seen;
  char buf[480];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  DD_LOCK;
  if (seen.insert(buf).second) patch::Log("d3d11: %s", buf);
}

void GpuTexture::Reset() {
  SafeRelease(srv);
  SafeRelease(rtv);
  SafeRelease(dsv);
  SafeRelease(tex);
  width = height = levels = 0;
  depth = false;
}

Backend& B() { return g_backend; }
bool Ready() { return g_backend.device != nullptr; }

ID3DBlob* Compile(const char* source, const char* entry, const char* target) {
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                     ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                     ID3DBlob**);
  static CompileFn compile = [] {
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    return m ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(m, "D3DCompile"))) : nullptr;
  }();
  if (!compile) {
    Log("d3dcompiler_47.dll not found");
    return nullptr;
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT hr = compile(source, std::strlen(source), "mcm2fix", nullptr, nullptr, entry, target,
                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    Log("shader %s failed: %s", entry,
        errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    SafeRelease(errors);
    return nullptr;
  }
  SafeRelease(errors);
  return code;
}

namespace {

IDXGIAdapter1* PickAdapter(IDXGIFactory1* factory) {
  IDXGIFactory6* f6 = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6)))) {
    f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                   __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    f6->Release();
  }
  if (!adapter) factory->EnumAdapters1(0, &adapter);
  return adapter;
}

bool CreatePipeline() {
  Backend& b = g_backend;
  ID3DBlob* vs = Compile(kBlitShader, "vs_main", "vs_4_0");
  ID3DBlob* ps = Compile(kBlitShader, "ps_main", "ps_4_0");
  if (!vs || !ps) {
    SafeRelease(vs);
    SafeRelease(ps);
    return false;
  }
  b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &b.blit_vs);
  b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &b.blit_ps);
  vs->Release();
  ps->Release();

  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(BlitCB);
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  b.device->CreateBuffer(&cb, nullptr, &b.blit_cb);

  D3D11_SAMPLER_DESC sd{};
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  b.device->CreateSamplerState(&sd, &b.point_sampler);
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  b.device->CreateSamplerState(&sd, &b.linear_sampler);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  b.device->CreateRasterizerState(&rd, &b.blit_raster);

  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = FALSE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  b.device->CreateDepthStencilState(&dsd, &b.no_depth);

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  b.device->CreateBlendState(&bd, &b.no_blend);
  return b.blit_vs && b.blit_ps && b.blit_cb;
}

}  // namespace

bool InitDevice() {
  DD_LOCK;
  Backend& b = g_backend;
  if (b.device) return true;
  if (g_device_failed) return false;
  g_device_failed = true;

  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    Log("CreateDXGIFactory1 failed");
    return false;
  }
  IDXGIAdapter1* adapter = PickAdapter(factory);
  factory->Release();
  if (adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, b.adapter_name,
                        sizeof(b.adapter_name), nullptr, nullptr);
    b.vendor_id = desc.VendorId;
    b.device_id = desc.DeviceId;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION,
                                 &b.device, &got, &b.ctx);
  SafeRelease(adapter);
  if (FAILED(hr)) {
    Log("D3D11CreateDevice failed %08lx", hr);
    return false;
  }
  if (!CreatePipeline()) {
    Log("blit pipeline failed");
    return false;
  }
  g_device_failed = false;
  Log("device on \"%s\" (vendor %04x device %04x), feature level %x", b.adapter_name,
      b.vendor_id, b.device_id, static_cast<unsigned>(got));
  return true;
}

namespace {

void ApplyBorderless(HWND hwnd) {
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoA(mon, &mi);
  g_monitor_rect = mi.rcMonitor;
  LONG style = GetWindowLongA(hwnd, GWL_STYLE);
  LONG want = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
                         WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME)) |
              WS_POPUP | WS_VISIBLE;
  if (style != want) SetWindowLongA(hwnd, GWL_STYLE, want);
  LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
  LONG want_ex = ex & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                        WS_EX_STATICEDGE);
  if (ex != want_ex) SetWindowLongA(hwnd, GWL_EXSTYLE, want_ex);
  RECT r{};
  GetWindowRect(hwnd, &r);
  if (style != want || ex != want_ex || !EqualRect(&r, &g_monitor_rect)) {
    SetWindowPos(hwnd, HWND_TOP, g_monitor_rect.left, g_monitor_rect.top,
                 g_monitor_rect.right - g_monitor_rect.left,
                 g_monitor_rect.bottom - g_monitor_rect.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
}

bool ResizeSwapchain() {
  Backend& b = g_backend;
  RECT rc{};
  GetClientRect(b.hwnd, &rc);
  UINT w = rc.right > 0 ? rc.right : 1, h = rc.bottom > 0 ? rc.bottom : 1;
  if (b.swapchain && w == b.swap_w && h == b.swap_h && b.swap_rtv) return true;
  SafeRelease(b.swap_rtv);
  if (b.swapchain) {
    b.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    b.ctx->Flush();
    if (FAILED(b.swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
  } else {
    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    b.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev));
    if (dxgi_dev) dxgi_dev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    SafeRelease(adapter);
    SafeRelease(dxgi_dev);
    if (!factory) {
      Log("no IDXGIFactory2");
      return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = factory->CreateSwapChainForHwnd(b.device, b.hwnd, &sd, nullptr, nullptr,
                                                 &b.swapchain);
    if (FAILED(hr)) {
      sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      sd.BufferCount = 1;
      hr = factory->CreateSwapChainForHwnd(b.device, b.hwnd, &sd, nullptr, nullptr,
                                           &b.swapchain);
    }
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(b.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    if (FAILED(hr)) {
      Log("CreateSwapChainForHwnd failed %08lx", hr);
      return false;
    }
  }
  ID3D11Texture2D* back = nullptr;
  b.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
  if (back) {
    b.device->CreateRenderTargetView(back, nullptr, &b.swap_rtv);
    back->Release();
  }
  b.swap_w = w;
  b.swap_h = h;
  LogOnce("swap chain %ux%u", w, h);
  return b.swap_rtv != nullptr;
}

}  // namespace

bool AttachWindow(HWND hwnd) {
  DD_LOCK;
  if (!InitDevice() || !hwnd) return false;
  Backend& b = g_backend;
  if (b.hwnd != hwnd) {
    SafeRelease(b.swap_rtv);
    SafeRelease(b.swapchain);
    b.swap_w = b.swap_h = 0;
    b.hwnd = hwnd;
  }
  ApplyBorderless(hwnd);
  return ResizeSwapchain();
}

void KeepBorderless() {
  if (g_backend.hwnd && !IsIconic(g_backend.hwnd)) ApplyBorderless(g_backend.hwnd);
}

bool CreateGpuTexture(GpuTexture& t, UINT w, UINT h, UINT levels, bool render_target,
                      bool depth) {
  t.Reset();
  Backend& b = g_backend;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w ? w : 1;
  td.Height = h ? h : 1;
  td.MipLevels = levels ? levels : 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  if (depth) {
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  } else {
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (render_target ? D3D11_BIND_RENDER_TARGET : 0);
  }
  if (FAILED(b.device->CreateTexture2D(&td, nullptr, &t.tex))) {
    Log("CreateTexture2D %ux%u levels %u failed", w, h, levels);
    return false;
  }
  if (depth) {
    b.device->CreateDepthStencilView(t.tex, nullptr, &t.dsv);
  } else {
    b.device->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    if (render_target) b.device->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
  }
  t.width = td.Width;
  t.height = td.Height;
  t.levels = td.MipLevels;
  t.depth = depth;
  return true;
}

void Blit(const BlitParams& p) {
  Backend& b = g_backend;
  if (!p.dst || !p.dst_w || !p.dst_h) return;
  BlitCB cb{};
  cb.dst[0] = 2.0f * p.dst_rect.left / p.dst_w - 1.0f;
  cb.dst[1] = 1.0f - 2.0f * p.dst_rect.top / p.dst_h;
  cb.dst[2] = 2.0f * p.dst_rect.right / p.dst_w - 1.0f;
  cb.dst[3] = 1.0f - 2.0f * p.dst_rect.bottom / p.dst_h;
  cb.src[0] = static_cast<float>(p.src_rect.left) / p.src_w;
  cb.src[1] = static_cast<float>(p.src_rect.top) / p.src_h;
  cb.src[2] = static_cast<float>(p.src_rect.right) / p.src_w;
  cb.src[3] = static_cast<float>(p.src_rect.bottom) / p.src_h;
  for (int i = 0; i < 4; ++i) cb.fill[i] = p.fill[i];
  cb.flags[0] = p.src ? 1.0f : 0.0f;
  cb.flags[1] = p.color_key ? 1.0f : 0.0f;
  cb.flags[2] = p.opaque ? 1.0f : 0.0f;
  for (int i = 0; i < 3; ++i) cb.key[i] = p.key[i];
  b.ctx->UpdateSubresource(b.blit_cb, 0, nullptr, &cb, 0, 0);

  ID3D11ShaderResourceView* null_srv[2] = {};
  b.ctx->PSSetShaderResources(0, 2, null_srv);
  b.ctx->OMSetRenderTargets(1, &p.dst, nullptr);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(p.dst_w), static_cast<float>(p.dst_h), 0, 1};
  b.ctx->RSSetViewports(1, &vp);
  b.ctx->RSSetState(b.blit_raster);
  b.ctx->OMSetDepthStencilState(b.no_depth, 0);
  b.ctx->OMSetBlendState(b.no_blend, nullptr, 0xffffffff);
  b.ctx->IASetInputLayout(nullptr);
  b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  b.ctx->VSSetShader(b.blit_vs, nullptr, 0);
  b.ctx->VSSetConstantBuffers(0, 1, &b.blit_cb);
  b.ctx->PSSetShader(b.blit_ps, nullptr, 0);
  b.ctx->PSSetConstantBuffers(0, 1, &b.blit_cb);
  ID3D11SamplerState* s = p.linear ? b.linear_sampler : b.point_sampler;
  b.ctx->PSSetSamplers(0, 1, &s);
  if (p.src) b.ctx->PSSetShaderResources(0, 1, &p.src);
  b.ctx->Draw(4, 0);
  if (p.src) b.ctx->PSSetShaderResources(0, 1, null_srv);
}

void Present(ID3D11ShaderResourceView* src, UINT src_w, UINT src_h, UINT interval) {
  DD_LOCK;
  Backend& b = g_backend;
  if (!b.hwnd || IsIconic(b.hwnd)) return;
  KeepBorderless();
  if (!ResizeSwapchain()) return;
  const float clear[4] = {0, 0, 0, 1};
  b.ctx->ClearRenderTargetView(b.swap_rtv, clear);
  if (src && src_w && src_h) {
    // Fit inside the window, keeping the game's aspect ratio.
    double sx = static_cast<double>(b.swap_w) / src_w, sy = static_cast<double>(b.swap_h) / src_h;
    double s = sx < sy ? sx : sy;
    LONG w = static_cast<LONG>(src_w * s + 0.5), h = static_cast<LONG>(src_h * s + 0.5);
    BlitParams p;
    p.dst = b.swap_rtv;
    p.dst_w = b.swap_w;
    p.dst_h = b.swap_h;
    p.dst_rect = {static_cast<LONG>(b.swap_w - w) / 2, static_cast<LONG>(b.swap_h - h) / 2, 0, 0};
    p.dst_rect.right = p.dst_rect.left + w;
    p.dst_rect.bottom = p.dst_rect.top + h;
    p.src = src;
    p.src_w = src_w;
    p.src_h = src_h;
    p.src_rect = {0, 0, static_cast<LONG>(src_w), static_cast<LONG>(src_h)};
    p.linear = true;
    Blit(p);
  }
  HRESULT hr = b.swapchain->Present(interval, 0);
  if (FAILED(hr)) LogOnce("Present failed %08lx", hr);
}

void WaitVBlank() {
  IDXGIOutput* out = nullptr;
  {
    DD_LOCK;
    if (g_backend.swapchain) g_backend.swapchain->GetContainingOutput(&out);
  }
  if (out) {
    out->WaitForVBlank();
    out->Release();
  } else {
    Sleep(1);
  }
}

}  // namespace dd
