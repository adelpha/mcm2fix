#include "backend9.h"
#include "gputext.h"
#include "msaa.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <string>

#include "../config.h"
#include "../patch.h"
#include "../prof.h"

namespace dd {

namespace {
Backend g_backend;
Api g_api = Api::kD3D11;
bool g_device_failed = false;
bool g_tearing = false;  // swap chain made with ALLOW_TEARING (VSync=0)
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
VSOut vs_quads(float2 pos : POSITION, float2 uv : TEXCOORD0) {
  VSOut o;
  o.pos = float4(pos, 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  if (flags.x == 0) return fill;
  float4 c = tex.Sample(smp, i.uv);
  if (flags.y != 0 && all(abs(c.rgb - key.rgb) < 0.002)) discard;
  if (flags.w != 0) return c * fill;
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
  patch::Log("%s: %s", g_api == Api::kD3D9 ? "d3d9" : "d3d11", buf);
}

void LogOnce(const char* fmt, ...) {
  // Some callers run every draw or every frame (fog state, swap chain size).
  // Formatting is the expensive part, so after 64 calls with the same format
  // string we stop formatting it: later new values of it are not logged.
  static std::set<std::string> seen;
  static std::unordered_map<const char*, unsigned> calls;
  DD_LOCK;
  unsigned& n = calls[fmt];
  if (n > 64) return;
  if (++n > 64) {
    patch::Log("%s: (LogOnce: \"%.60s\" called 64 times; not logging it any more)",
               g_api == Api::kD3D9 ? "d3d9" : "d3d11", fmt);
    return;
  }
  char buf[480];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (seen.insert(buf).second) patch::Log("%s: %s", g_api == Api::kD3D9 ? "d3d9" : "d3d11", buf);
}

Api ActiveApi() { return g_api; }

namespace {
unsigned g_ctx_epoch = 1;
}
void ContextTouched() { ++g_ctx_epoch; }
unsigned ContextEpoch() { return g_ctx_epoch; }

void GpuTexture::Reset() {
  if (const void* n = Native()) {
    MsaaForget(n);
    GpuTextForget(n);
  }
  SafeRelease(srv);
  SafeRelease(rtv);
  SafeRelease(dsv);
  SafeRelease(tex);
  SafeRelease(surf9);
  SafeRelease(tex9);
  fmt9 = 0;
  pool_default = false;
  width = height = levels = 0;
  depth = false;
  rt = false;
  gdi = false;
}

void BlitParams::SetSrc(const GpuTexture& t) {
  src = t.srv;
  src9 = t.tex9;
  src_w = t.width;
  src_h = t.height;
}

GpuTexture GpuTexture::Share() const {
  GpuTexture t = *this;
  if (t.tex) t.tex->AddRef();
  if (t.srv) t.srv->AddRef();
  if (t.rtv) t.rtv->AddRef();
  if (t.dsv) t.dsv->AddRef();
  if (t.tex9) t.tex9->AddRef();
  if (t.surf9) t.surf9->AddRef();
  return t;
}

Backend& B() { return g_backend; }
bool Ready() { return g_backend.device != nullptr || g_backend.dev9 != nullptr; }

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

// d3d11.dll and dxgi.dll are loaded at run time, so the DLL itself loads on
// systems without them (Windows XP), where the D3D9 renderer is used.
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                        const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                        D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
template <typename T>
T LoadFn(const char* dll, const char* name) {
  HMODULE m = LoadLibraryA(dll);
  return m ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

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
  // Batched quads (GPU text glyphs): same pixel shader, vertices from a buffer.
  if (ID3DBlob* qvs = Compile(kBlitShader, "vs_quads", "vs_4_0")) {
    const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    b.device->CreateVertexShader(qvs->GetBufferPointer(), qvs->GetBufferSize(), nullptr, &b.quad_vs);
    b.device->CreateInputLayout(el, 2, qvs->GetBufferPointer(), qvs->GetBufferSize(), &b.quad_layout);
    qvs->Release();
    D3D11_BUFFER_DESC vd{};
    vd.ByteWidth = kQuadBatch * 6 * 16;
    vd.Usage = D3D11_USAGE_DYNAMIC;
    vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    b.device->CreateBuffer(&vd, nullptr, &b.quad_vb);
  }
  if (!b.quad_vs || !b.quad_layout || !b.quad_vb) Log("batched blits unavailable: one draw per glyph");

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
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
  b.device->CreateBlendState(&bd, &b.alpha_only);
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  b.device->CreateBlendState(&bd, &b.alpha_blend);
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  b.device->CreateBlendState(&bd, &b.premul_blend);
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
  b.device->CreateBlendState(&bd, &b.erase_blend);
  return b.blit_vs && b.blit_ps && b.blit_cb;
}

}  // namespace

namespace {
bool InitDevice11();
}

bool InitDevice() {
  DD_LOCK;
  Backend& b = g_backend;
  if (b.device || b.dev9) return true;
  if (g_device_failed) return false;
  g_device_failed = true;
  const bool want9 = GetConfig().renderer == 9;
  patch::Log("renderer: %s asked for in mcm2fix.ini", want9 ? "Direct3D 9" : "Direct3D 11");
  if (!want9) {
    g_api = Api::kD3D11;
    if (InitDevice11()) {
      g_device_failed = false;
      return true;
    }
    patch::Log("renderer: Direct3D 11 not available; trying Direct3D 9");
  }
  g_api = Api::kD3D9;
  if (InitDevice9()) {
    g_device_failed = false;
    return true;
  }
  patch::Log("renderer: no usable Direct3D device");
  return false;
}

namespace {
bool InitDevice11() {
  Backend& b = g_backend;
  static const auto create_factory = LoadFn<CreateFactoryFn>("dxgi.dll", "CreateDXGIFactory1");
  static const auto create_device = LoadFn<CreateDeviceFn>("d3d11.dll", "D3D11CreateDevice");
  if (!create_factory || !create_device) {
    Log("d3d11.dll / dxgi.dll not found");
    return false;
  }
  IDXGIFactory1* factory = nullptr;
  if (FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
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
  HRESULT hr = create_device(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
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
    SafeRelease(b.ctx);
    SafeRelease(b.device);
    return false;
  }
  Log("device on \"%s\" (vendor %04x device %04x), feature level %x", b.adapter_name,
      b.vendor_id, b.device_id, static_cast<unsigned>(got));
  return true;
}
}  // namespace

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

namespace {

bool ResizeSwapchain() {
  Backend& b = g_backend;
  RECT rc{};
  GetClientRect(b.hwnd, &rc);
  UINT w = rc.right > 0 ? rc.right : 1, h = rc.bottom > 0 ? rc.bottom : 1;
  if (b.swapchain && w == b.swap_w && h == b.swap_h && b.swap_rtv) return true;
  SafeRelease(b.swap_rtv);
  if (b.swapchain) {
    b.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ContextTouched();
    b.ctx->Flush();
    if (FAILED(b.swapchain->ResizeBuffers(
            0, w, h, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
      return false;
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
    // With VSync off, let frames reach the screen as soon as they are ready
    // (tearing) instead of DWM holding them for the next refresh.
    g_tearing = false;
    if (!GetConfig().vsync) {
      IDXGIFactory5* f5 = nullptr;
      if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
          g_tearing = allow != FALSE;
        f5->Release();
      }
      if (g_tearing) sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      Log("VSync off, tearing %s", g_tearing ? "supported" : "not supported");
    }
    HRESULT hr = factory->CreateSwapChainForHwnd(b.device, b.hwnd, &sd, nullptr, nullptr,
                                                 &b.swapchain);
    if (FAILED(hr)) {
      g_tearing = false;
      sd.Flags = 0;
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
  // Show black straight away rather than whatever was behind the window
  // until the game's first frame arrives.
  if (b.swap_rtv) {
    const float black[4] = {0, 0, 0, 1};
    b.ctx->ClearRenderTargetView(b.swap_rtv, black);
    b.swapchain->Present(0, 0);
  }
  LogOnce("swap chain %ux%u", w, h);
  return b.swap_rtv != nullptr;
}

}  // namespace

bool AttachWindow(HWND hwnd) {
  DD_LOCK;
  if (!InitDevice() || !hwnd) return false;
  if (IsD3D9()) return AttachWindow9(hwnd);
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

bool CreateGpuTexture9(GpuTexture& t, UINT w, UINT h, UINT levels, bool render_target,
                       bool depth, bool alpha);

bool CreateGpuTexture(GpuTexture& t, UINT w, UINT h, UINT levels, bool render_target,
                      bool depth, bool alpha) {
  t.Reset();
  prof::Count(prof::cTexCreates);
  if (IsD3D9()) return CreateGpuTexture9(t, w, h, levels, render_target, depth, alpha);
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
    // Single-level render targets can take GDI drawing directly
    // (IDXGISurface1::GetDC), so GetDC needs no readback to the CPU.
    if (render_target && td.MipLevels == 1) td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
  }
  HRESULT hr = b.device->CreateTexture2D(&td, nullptr, &t.tex);
  if (FAILED(hr) && td.MiscFlags) {
    td.MiscFlags = 0;
    hr = b.device->CreateTexture2D(&td, nullptr, &t.tex);
  }
  if (FAILED(hr)) {
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
  t.rt = render_target && !depth;
  t.gdi = td.MiscFlags != 0;
  return true;
}

namespace {

// Everything of a blit but the shaders, vertices and the draw.
void SetupBlit(const BlitParams& p) {
  Backend& b = g_backend;
  ContextTouched();
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
  cb.flags[3] = p.tint ? 1.0f : 0.0f;
  for (int i = 0; i < 3; ++i) cb.key[i] = p.key[i];
  b.ctx->UpdateSubresource(b.blit_cb, 0, nullptr, &cb, 0, 0);

  ID3D11ShaderResourceView* null_srv[2] = {};
  b.ctx->PSSetShaderResources(0, 2, null_srv);
  b.ctx->OMSetRenderTargets(1, &p.dst, nullptr);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(p.dst_w), static_cast<float>(p.dst_h), 0, 1};
  b.ctx->RSSetViewports(1, &vp);
  b.ctx->RSSetState(b.blit_raster);
  b.ctx->OMSetDepthStencilState(b.no_depth, 0);
  b.ctx->OMSetBlendState(p.erase && b.erase_blend ? b.erase_blend
                         : p.premul && b.premul_blend ? b.premul_blend
                         : p.blend && b.alpha_blend ? b.alpha_blend
                         : p.alpha_only && b.alpha_only ? b.alpha_only : b.no_blend, nullptr,
                         0xffffffff);
  b.ctx->VSSetConstantBuffers(0, 1, &b.blit_cb);
  b.ctx->PSSetShader(b.blit_ps, nullptr, 0);
  b.ctx->PSSetConstantBuffers(0, 1, &b.blit_cb);
  ID3D11SamplerState* s = p.linear ? b.linear_sampler : b.point_sampler;
  b.ctx->PSSetSamplers(0, 1, &s);
  if (p.src) b.ctx->PSSetShaderResources(0, 1, &p.src);
}

}  // namespace

void Blit(const BlitParams& p) {
  if (IsD3D9()) return Blit9(p);
  Backend& b = g_backend;
  if (!p.dst || !p.dst_w || !p.dst_h) return;
  SetupBlit(p);
  b.ctx->IASetInputLayout(nullptr);
  b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  b.ctx->VSSetShader(b.blit_vs, nullptr, 0);
  b.ctx->Draw(4, 0);
  ID3D11ShaderResourceView* null_srv[1] = {};
  if (p.src) b.ctx->PSSetShaderResources(0, 1, null_srv);
}

void BlitQuads(const BlitParams& p, const BlitQuad* q, size_t n) {
  if (IsD3D9()) return BlitQuads9(p, q, n);
  Backend& b = g_backend;
  if (!p.dst || !p.dst_w || !p.dst_h || !n) return;
  if (!b.quad_vs || !b.quad_layout || !b.quad_vb) {
    BlitParams one = p;
    for (size_t i = 0; i < n; ++i) {
      one.dst_rect = q[i].dst;
      one.src_rect = q[i].src;
      Blit(one);
    }
    return;
  }
  SetupBlit(p);
  b.ctx->IASetInputLayout(b.quad_layout);
  b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const UINT stride = 16, offset = 0;
  b.ctx->IASetVertexBuffers(0, 1, &b.quad_vb, &stride, &offset);
  b.ctx->VSSetShader(b.quad_vs, nullptr, 0);
  const float kx = 2.0f / p.dst_w, ky = 2.0f / p.dst_h;
  const float ku = 1.0f / p.src_w, kv = 1.0f / p.src_h;
  for (size_t done = 0; done < n;) {
    const size_t m = std::min<size_t>(n - done, kQuadBatch);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(b.ctx->Map(b.quad_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) break;
    float* v = static_cast<float*>(map.pData);
    for (size_t i = 0; i < m; ++i) {
      const BlitQuad& e = q[done + i];
      const float x0 = e.dst.left * kx - 1.0f, x1 = e.dst.right * kx - 1.0f;
      const float y0 = 1.0f - e.dst.top * ky, y1 = 1.0f - e.dst.bottom * ky;
      const float u0 = e.src.left * ku, u1 = e.src.right * ku, v0 = e.src.top * kv, v1 = e.src.bottom * kv;
      const float c[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                             {x0, y1, u0, v1}, {x1, y0, u1, v0}, {x1, y1, u1, v1}};
      std::memcpy(v, c, sizeof(c));
      v += 24;
    }
    b.ctx->Unmap(b.quad_vb, 0);
    b.ctx->Draw(static_cast<UINT>(m * 6), 0);
    done += m;
  }
  ID3D11ShaderResourceView* null_srv[1] = {};
  if (p.src) b.ctx->PSSetShaderResources(0, 1, null_srv);
}

Stats& Counters() {
  static Stats s{};
  return s;
}

namespace {
std::map<std::string, unsigned> g_readbacks;
}

void NoteReadback(const char* why, DWORD caps, UINT w, UINT h) {
  if (!GetConfig().dd_stats) return;
  char key[96];
  std::snprintf(key, sizeof(key), "%s caps=%lx %ux%u", why, caps, w, h);
  ++g_readbacks[key];
}

void ReportStats() {
  static DWORD last = GetTickCount();
  static int reports = 0;
  if (!GetConfig().dd_stats) return;
  DWORD now = GetTickCount();
  if (now - last < 2000 || reports > 200) return;
  Stats& s = Counters();
  Log("2s: %u flips, %u presents, %u draws, %u blts, %u video frames (sum %08x), %u front writes, "
      "%u locks, %u readbacks",
      s.flips, s.presents, s.draws, s.blts, s.foreign, s.foreign_sum, s.front_writes, s.locks,
      s.downloads);
  if (!g_readbacks.empty()) {
    std::string line;
    for (const auto& kv : g_readbacks) {
      char part[128];
      std::snprintf(part, sizeof(part), "%s%s x%u", line.empty() ? "" : "; ", kv.first.c_str(), kv.second);
      line += part;
    }
    Log("   readbacks: %.400s", line.c_str());
    g_readbacks.clear();
  }
  unsigned sum = s.foreign_sum;
  s = Stats{};
  s.foreign_sum = sum;
  last = now;
  ++reports;
}

namespace {
// GPU time per frame (Profile=1): timestamp queries from the frame's first
// draw to the end of its Present, read a few frames later.
struct GpuTimer11 {
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* begin = nullptr;
  ID3D11Query* end = nullptr;
  bool pending = false;
};
GpuTimer11 g_gpu_timers[6];
unsigned g_gpu_timer_i = 0;
bool g_gpu_timer_open = false;
bool g_gpu_timer_begun = false;

void GpuTimerCollect11() {
  Backend& b = g_backend;
  for (GpuTimer11& t : g_gpu_timers) {
    if (!t.pending) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 t0 = 0, t1 = 0;
    if (b.ctx->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (b.ctx->GetData(t.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (b.ctx->GetData(t.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    t.pending = false;
    if (!dj.Disjoint && dj.Frequency && t1 >= t0)
      prof::GpuFrameTime(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency));
  }
}

// Called right after Present: close this frame's timer, open the next.
void GpuTimerFrame11() {
  if (!prof::On()) return;
  Backend& b = g_backend;
  if (g_gpu_timer_open) {
    GpuTimer11& t = g_gpu_timers[g_gpu_timer_i % 6];
    if (!g_gpu_timer_begun) b.ctx->End(t.begin);
    b.ctx->End(t.end);
    b.ctx->End(t.disjoint);
    t.pending = true;
    g_gpu_timer_open = false;
    ++g_gpu_timer_i;
  }
  GpuTimerCollect11();
  GpuTimer11& n = g_gpu_timers[g_gpu_timer_i % 6];
  if (n.pending) return;  // GPU far behind: skip a frame
  if (!n.disjoint) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    b.device->CreateQuery(&qd, &n.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    b.device->CreateQuery(&qd, &n.begin);
    b.device->CreateQuery(&qd, &n.end);
    if (!n.disjoint || !n.begin || !n.end) return;
  }
  b.ctx->Begin(n.disjoint);
  g_gpu_timer_open = true;
  g_gpu_timer_begun = false;  // the begin stamp goes in at the frame's first draw
}
}  // namespace

void GpuTimerMark() {
  if (IsD3D9()) return GpuTimerMark9();
  if (!g_gpu_timer_open || g_gpu_timer_begun) return;
  g_gpu_timer_begun = true;
  g_backend.ctx->End(g_gpu_timers[g_gpu_timer_i % 6].begin);
}

void Present(const GpuTexture& src_tex, UINT src_w, UINT src_h, UINT interval) {
  DD_LOCK;
  if (IsD3D9()) return Present9(src_tex, src_w, src_h, interval);
  prof::Scope zone(prof::kCompose);
  ID3D11ShaderResourceView* src = src_tex.srv;
  Backend& b = g_backend;
  ++Counters().presents;
  ReportStats();
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
    GpuTextComposite(src_tex, b.swap_rtv, nullptr, b.swap_w, b.swap_h, p.dst_rect);
  }
  DrawRendererLabel(b.swap_rtv, nullptr, b.swap_w, b.swap_h);
  zone.Switch(prof::kSwapPresent);
  HRESULT hr = b.swapchain->Present(interval, interval == 0 && g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
  if (FAILED(hr)) LogOnce("Present failed %08lx", hr);
  zone.Switch(prof::kCompose);
  GpuTimerFrame11();
}

void WaitVBlank() {
  if (IsD3D9()) return WaitVBlank9();
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

// ------------------------------------------------------------- renderer label

namespace {
DWORD g_last_3d = 0;
bool g_any_3d = false;
}  // namespace

void Note3DDraw() {
  g_last_3d = GetTickCount();
  g_any_3d = true;
}

void DrawRendererLabel(ID3D11RenderTargetView* dst, IDirect3DSurface9* dst9, UINT w, UINT h) {
  if (!GetConfig().renderer_label || (!dst && !dst9) || w < 64 || h < 64) return;
  // Menus: no 3D scene drawn for half a second.
  if (g_any_3d && GetTickCount() - g_last_3d < 500) return;
  static GpuTexture label;
  static UINT label_h = 0;
  static Api label_api = Api::kD3D11;
  static bool failed = false;
  const UINT font_h = h / 54 < 12 ? 12 : h / 54;
  if (!label.Valid() || label_h != font_h || label_api != ActiveApi()) {
    if (failed) return;
    label.Reset();
    label_h = font_h;
    label_api = ActiveApi();
    const char* text = IsD3D9() ? "Direct3D 9" : "Direct3D 11";
    // Draw the text with GDI (white on black), then turn brightness into
    // a white label on a translucent dark box.
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontA(-static_cast<int>(font_h), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, "Tahoma");
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32A(dc, text, static_cast<int>(std::strlen(text)), &sz);
    const int pad = static_cast<int>(font_h) / 3 + 2;
    const int tw = sz.cx + pad * 2, th = sz.cy + pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
      HGDIOBJ old_bmp = SelectObject(dc, bmp);
      std::memset(bits, 0, static_cast<size_t>(tw) * th * 4);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      TextOutA(dc, pad, pad / 2, text, static_cast<int>(std::strlen(text)));
      GdiFlush();
      auto* px = static_cast<uint8_t*>(bits);
      for (int i = 0; i < tw * th; ++i) {
        const uint8_t v = std::max(px[i * 4], std::max(px[i * 4 + 1], px[i * 4 + 2]));
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = v;
        px[i * 4 + 3] = static_cast<uint8_t>(110 + v * (255 - 110) / 255);
      }
      if (!CreateTextureFromBGRA(label, px, static_cast<UINT>(tw), static_cast<UINT>(th))) failed = true;
      SelectObject(dc, old_bmp);
    } else {
      failed = true;
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    if (failed) {
      Log("renderer label could not be made");
      return;
    }
    LogOnce("renderer label \"%s\" %ux%u", text, label.width, label.height);
  }
  const LONG margin = static_cast<LONG>(font_h) / 2 + 4;
  BlitParams p;
  p.dst = dst;
  p.dst9 = dst9;
  p.dst_w = w;
  p.dst_h = h;
  p.dst_rect = {static_cast<LONG>(w) - margin - static_cast<LONG>(label.width),
                static_cast<LONG>(h) - margin - static_cast<LONG>(label.height), 0, 0};
  p.dst_rect.right = p.dst_rect.left + static_cast<LONG>(label.width);
  p.dst_rect.bottom = p.dst_rect.top + static_cast<LONG>(label.height);
  p.SetSrc(label);
  p.src_rect = {0, 0, static_cast<LONG>(label.width), static_cast<LONG>(label.height)};
  p.opaque = false;
  p.blend = true;
  Blit(p);
}

// ---------------------------------------------------------------- texture I/O

bool CreateTextureFromBGRA9(GpuTexture& t, const uint8_t* bgra, UINT w, UINT h);
void UploadLevel9(GpuTexture& t, UINT level, const uint8_t* bgra, UINT w, UINT h);
bool DownloadTop9(GpuTexture& t, void (*row)(void*, UINT, const uint8_t*), void* ctx);
bool CopyTexture9(GpuTexture& dst, const GpuTexture& src);
void ClearDepth9(GpuTexture& t, float z);
bool GpuGetDC9(GpuTexture& t, HDC* dc);
void GpuReleaseDC9(GpuTexture& t, HDC dc);

bool CreateTextureFromBGRA(GpuTexture& out, const uint8_t* px, UINT w, UINT h) {
  out.Reset();
  if (IsD3D9()) return CreateTextureFromBGRA9(out, px, w, h);
  Backend& b = g_backend;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 0;  // full chain
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (FAILED(b.device->CreateTexture2D(&td, nullptr, &out.tex))) return false;
  out.tex->GetDesc(&td);
  b.device->CreateShaderResourceView(out.tex, nullptr, &out.srv);
  if (!out.srv) {
    out.Reset();
    return false;
  }
  b.ctx->UpdateSubresource(out.tex, 0, nullptr, px, w * 4, 0);
  b.ctx->GenerateMips(out.srv);
  out.width = w;
  out.height = h;
  out.levels = td.MipLevels;
  return true;
}

void UploadLevel(GpuTexture& t, UINT level, const uint8_t* bgra, UINT w, UINT h) {
  if (IsD3D9()) return UploadLevel9(t, level, bgra, w, h);
  if (t.tex) g_backend.ctx->UpdateSubresource(t.tex, level, nullptr, bgra, w * 4, 0);
}

bool DownloadTop(GpuTexture& t, void (*row)(void*, UINT, const uint8_t*), void* ctx) {
  if (IsD3D9()) return DownloadTop9(t, row, ctx);
  if (!t.tex) return false;
  Backend& b = g_backend;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = t.width;
  td.Height = t.height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  // One reusable readback texture per size.
  static std::map<uint64_t, ID3D11Texture2D*> cache;
  ID3D11Texture2D*& staging = cache[(static_cast<uint64_t>(td.Width) << 32) | td.Height];
  if (!staging && FAILED(b.device->CreateTexture2D(&td, nullptr, &staging))) {
    staging = nullptr;
    Log("readback texture %ux%u failed", t.width, t.height);
    return false;
  }
  b.ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, t.tex, 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(b.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return false;
  for (UINT y = 0; y < t.height; ++y)
    row(ctx, y, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch);
  b.ctx->Unmap(staging, 0);
  return true;
}

bool CopyTexture(GpuTexture& dst, const GpuTexture& src) {
  if (IsD3D9()) return CopyTexture9(dst, src);
  if (!src.tex || !CreateGpuTexture(dst, src.width, src.height, 1, false, false)) return false;
  g_backend.ctx->CopySubresourceRegion(dst.tex, 0, 0, 0, 0, src.tex, 0, nullptr);
  return true;
}

void ClearDepth(GpuTexture& t, float z) {
  if (IsD3D9()) return ClearDepth9(t, z);
  if (t.dsv) g_backend.ctx->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH, z, 0);
}

bool GpuGetDC(GpuTexture& g, HDC* out, void** handle) {
  *handle = nullptr;
  if (IsD3D9()) return GpuGetDC9(g, out);
  if (!g.tex || !g.gdi) return false;
  IDXGISurface1* s = nullptr;
  if (FAILED(g.tex->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&s))))
    return false;
  // The texture must not be bound while GDI owns it; the device and the
  // blitter bind their targets again before every draw.
  ID3D11ShaderResourceView* null_srv[8] = {};
  g_backend.ctx->PSSetShaderResources(0, 8, null_srv);
  g_backend.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  ContextTouched();
  HDC dc = nullptr;
  if (FAILED(s->GetDC(FALSE, &dc)) || !dc) {
    LogOnce("IDXGISurface1::GetDC failed; using CPU copies for GDI");
    s->Release();
    return false;
  }
  *handle = s;
  *out = dc;
  return true;
}

void GpuReleaseDC(GpuTexture& g, HDC dc, void* handle) {
  if (IsD3D9()) return GpuReleaseDC9(g, dc);
  auto* s = static_cast<IDXGISurface1*>(handle);
  if (!s) return;
  s->ReleaseDC(nullptr);
  s->Release();
}

}  // namespace dd
