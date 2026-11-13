// Direct3D 9 renderer core (ini Renderer=D3D9, or when Direct3D 11 is
// missing): device, swap chains on the game window, texture plumbing, the 2D
// blitter and presenting.
//
// Everything is fixed-function Direct3D 9: no shaders, so no shader compiler
// is needed and it runs on Windows XP's d3d9.dll. d3d9.dll is loaded at run
// time; Direct3D9Ex is not used.
//
// Resources: textures the game only samples are D3DPOOL_MANAGED (they survive
// a device reset). Render targets and depth buffers live in D3DPOOL_DEFAULT
// and are dropped on a reset (Surface::DeviceLost9); their CPU copies are
// uploaded again on next use.
#include "backend9.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <cmath>
#include <vector>

#include "../config.h"
#include "../patch.h"
#include "../prof.h"
#include "gdihook.h"
#include "gputext.h"
#include "motionblur.h"
#include "msaa.h"
#include "surface.h"

void PerfBeforePresent();  // fixes_perf.cpp

namespace dd {

void UploadLevel9(GpuTexture& t, UINT level, const uint8_t* bgra, UINT w, UINT h);

namespace {

using Create9Fn = IDirect3D9*(WINAPI*)(UINT);

D3DCAPS9 g_caps{};
HWND g_dummy = nullptr;  // hidden window the device is created on
D3DPRESENT_PARAMETERS g_pp{};
DWORD g_behavior = 0;
bool g_lost = false;
bool g_in_scene = false;
void ReleaseGdi9();
unsigned g_resets = 0;

// Two swap chains on the game window: Present waits for the refresh on one
// (VSync) and not on the other (front-buffer writes, VSync=0).
struct Chains {
  HWND hwnd = nullptr;
  UINT w = 0, h = 0;
  IDirect3DSwapChain9* vsync = nullptr;
  IDirect3DSwapChain9* now = nullptr;
  bool failed = false;
} g_sc;
IDirect3DQuery9* g_vsync_done = nullptr;  // event query for manual VSync (Present9)
// Exclusive fullscreen (ini Fullscreen=Exclusive): the device is recreated on
// the game window and presents through its own (implicit) swap chain.
bool g_exclusive = false;
bool g_exclusive_tried = false;

// Cached device state.
struct StateCache {
  DWORD rs[256];
  bool rs_ok[256];
  DWORD tss[8][33];
  bool tss_ok[8][33];
  DWORD ss[8][14];
  bool ss_ok[8][14];
  IDirect3DBaseTexture9* tex[8];
  bool tex_ok[8];
  IDirect3DSurface9* rt;
  bool rt_ok;
  IDirect3DSurface9* ds;
  bool ds_ok;
  D3DVIEWPORT9 vp;
  bool vp_ok;
  RECT sc;
  bool sc_ok;
} g_state;

// Sysmem staging surfaces for uploads to / readbacks from video memory.
std::map<uint64_t, IDirect3DSurface9*> g_staging;
// Render target for clearing a depth buffer on its own (D3D9 needs one bound).
std::map<uint64_t, IDirect3DSurface9*> g_depth_clear_rt;
IDirect3DTexture9* g_white = nullptr;
std::map<uint64_t, IDirect3DTexture9*> g_upload_tex;  // UpdateSurface fallback

struct Stats9 {
  unsigned tex_managed = 0, tex_rt = 0, depth = 0, create_failed = 0;
  unsigned uploads = 0, upload_bytes = 0, readbacks = 0, blits = 0, presents = 0;
  unsigned gdi_gpu = 0, gdi_fail = 0;
} g_stats;

uint64_t Key(UINT w, UINT h, D3DFORMAT f) {
  return (static_cast<uint64_t>(w) << 40) ^ (static_cast<uint64_t>(h) << 16) ^ static_cast<uint64_t>(f);
}

IDirect3DSurface9* Staging(UINT w, UINT h, D3DFORMAT fmt) {
  IDirect3DSurface9*& s = g_staging[Key(w, h, fmt)];
  if (!s) {
    HRESULT hr = B().dev9->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &s, nullptr);
    if (FAILED(hr)) {
      s = nullptr;
      Log("staging surface %ux%u format %u failed %08lx", w, h, static_cast<unsigned>(fmt), hr);
    }
  }
  return s;
}

void ReleaseChains() {
  SafeRelease(g_sc.vsync);
  SafeRelease(g_vsync_done);
  SafeRelease(g_sc.now);
  g_sc.w = g_sc.h = 0;
}

HWND DummyWindow() {
  if (g_dummy) return g_dummy;
  WNDCLASSA wc{};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "mcm2fix_d3d9";
  RegisterClassA(&wc);
  g_dummy = CreateWindowExA(0, wc.lpszClassName, "mcm2fix", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            wc.hInstance, nullptr);
  return g_dummy;
}

const char* FormatName(D3DFORMAT f) {
  switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D16: return "D16";
    default: return "other";
  }
}

void LogCaps(const D3DCAPS9& c) {
  Log("caps: max texture %lux%lu, aspect %lu, anisotropy %lu, %lu texture stages / %lu textures",
      c.MaxTextureWidth, c.MaxTextureHeight, c.MaxTextureAspectRatio, c.MaxAnisotropy,
      c.MaxTextureBlendStages, c.MaxSimultaneousTextures);
  Log("caps: raster %08lx (wfog %d, zfog %d, depthbias %d, slopebias %d, scissor %d, aniso %d), "
      "texture %08lx (pow2 %d, nonpow2cond %d), filter %08lx",
      c.RasterCaps, (c.RasterCaps & D3DPRASTERCAPS_WFOG) != 0, (c.RasterCaps & D3DPRASTERCAPS_ZFOG) != 0,
      (c.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS) != 0, (c.RasterCaps & D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS) != 0,
      (c.RasterCaps & D3DPRASTERCAPS_SCISSORTEST) != 0, (c.RasterCaps & D3DPRASTERCAPS_ANISOTROPY) != 0,
      c.TextureCaps, (c.TextureCaps & D3DPTEXTURECAPS_POW2) != 0,
      (c.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0, c.TextureFilterCaps);
  Log("caps: devcaps %08lx (hw t&l %d), texop %08lx, blend src %08lx dst %08lx, vs %lu.%lu ps %lu.%lu",
      c.DevCaps, (c.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0, c.TextureOpCaps, c.SrcBlendCaps,
      c.DestBlendCaps, (c.VertexShaderVersion >> 8) & 0xff, c.VertexShaderVersion & 0xff,
      (c.PixelShaderVersion >> 8) & 0xff, c.PixelShaderVersion & 0xff);
}

bool CreateDevice9(DWORD vp, HWND focus = nullptr) {
  Backend& b = B();
  g_behavior = vp | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
  HRESULT hr = b.d3d9->CreateDevice(b.adapter9, D3DDEVTYPE_HAL, focus ? focus : DummyWindow(), g_behavior, &g_pp,
                                    &b.dev9);
  if (FAILED(hr)) {
    Log("CreateDevice (%s vertex processing, %s) failed %08lx",
        vp == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software",
        g_pp.Windowed ? "windowed" : "exclusive fullscreen", hr);
    b.dev9 = nullptr;
    return false;
  }
  return true;
}

void ResetState() {
  std::memset(&g_state, 0, sizeof(g_state));
}

void GpuTimerRelease9();

// Device reset after a loss: drop everything in video memory, reset, and
// let the next frame recreate it.
bool TryRecover() {
  Backend& b = B();
  HRESULT hr = b.dev9->TestCooperativeLevel();
  if (hr == S_OK) {
    g_lost = false;
    return true;
  }
  if (hr == D3DERR_DEVICELOST) return false;  // not yet
  if (hr != D3DERR_DEVICENOTRESET) {
    LogOnce("TestCooperativeLevel %08lx", hr);
    return false;
  }
  Log("device lost; resetting (reset #%u)", g_resets + 1);
  ReleaseChains();
  MsaaDeviceLost();
  MotionBlurDeviceLost();
  ReleaseGdi9();
  GpuTextDeviceLost();
  for (auto& kv : g_depth_clear_rt) SafeRelease(kv.second);
  g_depth_clear_rt.clear();
  Surface::DeviceLost9();
  GpuTimerRelease9();
  EndScene9();
  hr = b.dev9->Reset(&g_pp);
  if (FAILED(hr)) {
    LogOnce("Reset failed %08lx (will retry)", hr);
    return false;
  }
  ++g_resets;
  ResetState();
  for (auto& kv : g_upload_tex) SafeRelease(kv.second);
  g_upload_tex.clear();
  g_lost = false;
  Log("device reset done");
  return true;
}

bool EnsureChains() {
  Backend& b = B();
  if (!b.hwnd) return false;
  if (g_exclusive) {
    if (g_sc.vsync) return true;
    if (FAILED(b.dev9->GetSwapChain(0, &g_sc.vsync)) || !g_sc.vsync) {
      LogOnce("exclusive: GetSwapChain failed");
      return false;
    }
    g_sc.now = g_sc.vsync;
    g_sc.now->AddRef();
    g_sc.hwnd = b.hwnd;
    g_sc.w = g_pp.BackBufferWidth;
    g_sc.h = g_pp.BackBufferHeight;
    Log("exclusive: presenting on the device's own swap chain %ux%u, interval %s", g_sc.w, g_sc.h,
        g_pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE ? "ONE (VSync)" : "IMMEDIATE");
    return true;
  }
  RECT rc{};
  GetClientRect(b.hwnd, &rc);
  UINT w = rc.right > 0 ? rc.right : 1, h = rc.bottom > 0 ? rc.bottom : 1;
  if (g_sc.vsync && g_sc.now && g_sc.hwnd == b.hwnd && g_sc.w == w && g_sc.h == h) return true;
  ReleaseChains();
  g_sc.hwnd = b.hwnd;
  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = w;
  pp.BackBufferHeight = h;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;  // the desktop's
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = b.hwnd;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  HRESULT hr = b.dev9->CreateAdditionalSwapChain(&pp, &g_sc.vsync);
  if (SUCCEEDED(hr)) {
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = b.dev9->CreateAdditionalSwapChain(&pp, &g_sc.now);
  }
  if (FAILED(hr)) {
    LogOnce("CreateAdditionalSwapChain %ux%u failed %08lx", w, h, hr);
    ReleaseChains();
    return false;
  }
  g_sc.w = w;
  g_sc.h = h;
  D3DPRESENT_PARAMETERS got{};
  g_sc.vsync->GetPresentParameters(&got);
  Log("swap chains %ux%u, back buffer %s", w, h, FormatName(got.BackBufferFormat));
  // Black straight away.
  IDirect3DSurface9* back = nullptr;
  if (SUCCEEDED(g_sc.now->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back))) {
    EndScene9();
    b.dev9->ColorFill(back, nullptr, D3DCOLOR_XRGB(0, 0, 0));
    back->Release();
    g_sc.now->Present(nullptr, nullptr, nullptr, nullptr, 0);
  }
  return true;
}

void SetRT(IDirect3DSurface9* s) {
  // (SetRenderTarget also resets the viewport and scissor; callers set them after.)
  if (g_state.rt_ok && g_state.rt == s) return;
  B().dev9->SetRenderTarget(0, s);
  g_state.rt = s;
  g_state.rt_ok = s != nullptr;
  g_state.vp_ok = g_state.sc_ok = false;
}

void BoxDown(const std::vector<uint8_t>& src, UINT sw, UINT sh, std::vector<uint8_t>& dst, UINT& dw,
             UINT& dh) {
  dw = sw > 1 ? sw / 2 : 1;
  dh = sh > 1 ? sh / 2 : 1;
  dst.resize(static_cast<size_t>(dw) * dh * 4);
  for (UINT y = 0; y < dh; ++y) {
    for (UINT x = 0; x < dw; ++x) {
      UINT sx0 = x * 2 < sw ? x * 2 : sw - 1, sx1 = x * 2 + 1 < sw ? x * 2 + 1 : sw - 1;
      UINT sy0 = y * 2 < sh ? y * 2 : sh - 1, sy1 = y * 2 + 1 < sh ? y * 2 + 1 : sh - 1;
      for (int c = 0; c < 4; ++c) {
        unsigned sum = src[(static_cast<size_t>(sy0) * sw + sx0) * 4 + c] +
                       src[(static_cast<size_t>(sy0) * sw + sx1) * 4 + c] +
                       src[(static_cast<size_t>(sy1) * sw + sx0) * 4 + c] +
                       src[(static_cast<size_t>(sy1) * sw + sx1) * 4 + c];
        dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- state cache

void RS9(D3DRENDERSTATETYPE s, DWORD v) {
  const unsigned i = static_cast<unsigned>(s);
  if (i < 256 && g_state.rs_ok[i] && g_state.rs[i] == v) return;
  B().dev9->SetRenderState(s, v);
  if (i < 256) {
    g_state.rs[i] = v;
    g_state.rs_ok[i] = true;
  }
}

void TSS9(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
  const unsigned i = static_cast<unsigned>(t);
  if (stage < 8 && i < 33 && g_state.tss_ok[stage][i] && g_state.tss[stage][i] == v) return;
  B().dev9->SetTextureStageState(stage, t, v);
  if (stage < 8 && i < 33) {
    g_state.tss[stage][i] = v;
    g_state.tss_ok[stage][i] = true;
  }
}

void SS9(DWORD stage, D3DSAMPLERSTATETYPE t, DWORD v) {
  const unsigned i = static_cast<unsigned>(t);
  if (stage < 8 && i < 14 && g_state.ss_ok[stage][i] && g_state.ss[stage][i] == v) return;
  B().dev9->SetSamplerState(stage, t, v);
  if (stage < 8 && i < 14) {
    g_state.ss[stage][i] = v;
    g_state.ss_ok[stage][i] = true;
  }
}

void Tex9(DWORD stage, IDirect3DBaseTexture9* t) {
  if (stage < 8 && g_state.tex_ok[stage] && g_state.tex[stage] == t) return;
  B().dev9->SetTexture(stage, t);
  if (stage < 8) {
    g_state.tex[stage] = t;
    g_state.tex_ok[stage] = true;
  }
}

void InvalidateState9() { ResetState(); }

void SetTarget9(IDirect3DSurface9* rt, IDirect3DSurface9* ds) {
  SetRT(rt);
  SetDS9(ds);
}

void SetDS9(IDirect3DSurface9* ds) {
  if (g_state.ds_ok && g_state.ds == ds) return;
  B().dev9->SetDepthStencilSurface(ds);
  g_state.ds = ds;
  g_state.ds_ok = true;
}

void SetViewport9(const D3DVIEWPORT9& vp) {
  if (g_state.vp_ok && std::memcmp(&g_state.vp, &vp, sizeof(vp)) == 0) return;
  B().dev9->SetViewport(&vp);
  g_state.vp = vp;
  g_state.vp_ok = true;
}

void SetScissor9(const RECT& r) {
  if (g_state.sc_ok && EqualRect(&g_state.sc, &r)) return;
  B().dev9->SetScissorRect(&r);
  g_state.sc = r;
  g_state.sc_ok = true;
}

const D3DCAPS9& Caps9() { return g_caps; }

bool DeviceLost9() { return g_lost; }

unsigned ResetCount9() { return g_resets; }

IDirect3DTexture9* WhiteTex9() {
  if (!g_white && B().dev9) {
    if (SUCCEEDED(B().dev9->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_white, nullptr))) {
      D3DLOCKED_RECT lr{};
      if (SUCCEEDED(g_white->LockRect(0, &lr, nullptr, 0))) {
        *static_cast<DWORD*>(lr.pBits) = 0xffffffff;
        g_white->UnlockRect(0);
      }
    }
  }
  return g_white;
}

// --------------------------------------------------------------------- device

bool InitDevice9() {
  Backend& b = B();
  static const auto create = [] {
    HMODULE m = LoadLibraryA("d3d9.dll");
    return m ? reinterpret_cast<Create9Fn>(reinterpret_cast<void*>(GetProcAddress(m, "Direct3DCreate9")))
             : nullptr;
  }();
  if (!create) {
    Log("d3d9.dll / Direct3DCreate9 not found");
    return false;
  }
  b.d3d9 = create(D3D_SDK_VERSION);
  if (!b.d3d9) {
    Log("Direct3DCreate9 failed");
    return false;
  }
  const UINT count = b.d3d9->GetAdapterCount();
  b.adapter9 = D3DADAPTER_DEFAULT;
  for (UINT i = 0; i < count; ++i) {
    D3DADAPTER_IDENTIFIER9 id{};
    b.d3d9->GetAdapterIdentifier(i, 0, &id);
    Log("adapter %u: \"%s\" vendor %04lx device %04lx driver %s %u.%u.%u.%u", i, id.Description,
        id.VendorId, id.DeviceId, id.Driver, HIWORD(id.DriverVersion.HighPart),
        LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart),
        LOWORD(id.DriverVersion.LowPart));
    if (i == b.adapter9) {
      std::snprintf(b.adapter_name, sizeof(b.adapter_name), "%s", id.Description);
      b.vendor_id = id.VendorId;
      b.device_id = id.DeviceId;
    }
  }
  if (FAILED(b.d3d9->GetDeviceCaps(b.adapter9, D3DDEVTYPE_HAL, &g_caps))) {
    Log("no hardware Direct3D 9 device on adapter %u", b.adapter9);
    SafeRelease(b.d3d9);
    return false;
  }
  LogCaps(g_caps);
  g_pp = D3DPRESENT_PARAMETERS{};
  g_pp.BackBufferWidth = 1;
  g_pp.BackBufferHeight = 1;
  g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
  g_pp.BackBufferCount = 1;
  g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  g_pp.hDeviceWindow = DummyWindow();
  g_pp.Windowed = TRUE;
  g_pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  const bool hw_tl = (g_caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
  if (!(hw_tl && CreateDevice9(D3DCREATE_HARDWARE_VERTEXPROCESSING)) &&
      !CreateDevice9(D3DCREATE_SOFTWARE_VERTEXPROCESSING)) {
    SafeRelease(b.d3d9);
    return false;
  }
  ResetState();
  Log("device on \"%s\" (vendor %04x device %04x), behaviour %08lx", b.adapter_name, b.vendor_id,
      b.device_id, g_behavior);
  return true;
}

namespace {

// Picks the fullscreen mode: FullscreenMode if the card has it (closest
// refresh rate), else the desktop's.
D3DDISPLAYMODE PickMode9() {
  Backend& b = B();
  const Config& c = GetConfig();
  D3DDISPLAYMODE desk{};
  b.d3d9->GetAdapterDisplayMode(b.adapter9, &desk);
  D3DDISPLAYMODE pick = desk;
  pick.Format = D3DFMT_X8R8G8B8;
  if (c.fs_width) {
    const UINT n = b.d3d9->GetAdapterModeCount(b.adapter9, D3DFMT_X8R8G8B8);
    bool found = false;
    for (UINT i = 0; i < n; ++i) {
      D3DDISPLAYMODE m{};
      if (FAILED(b.d3d9->EnumAdapterModes(b.adapter9, D3DFMT_X8R8G8B8, i, &m))) continue;
      if (m.Width != static_cast<UINT>(c.fs_width) || m.Height != static_cast<UINT>(c.fs_height)) continue;
      // Wanted refresh rate, or else the highest.
      const int want = c.fs_hz ? c.fs_hz : 100000;
      if (!found || std::abs(static_cast<int>(m.RefreshRate) - want) <
                        std::abs(static_cast<int>(pick.RefreshRate) - want)) {
        pick = m;
        found = true;
      }
    }
    Log("exclusive: FullscreenMode %dx%d@%d -> %s %ux%u@%u", c.fs_width, c.fs_height, c.fs_hz,
        found ? "mode" : "not available, desktop", pick.Width, pick.Height, pick.RefreshRate);
  }
  return pick;
}

D3DPRESENT_PARAMETERS FullscreenParams9(HWND hwnd, const D3DDISPLAYMODE& m) {
  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = m.Width;
  pp.BackBufferHeight = m.Height;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = FALSE;
  pp.FullScreen_RefreshRateInHz = m.RefreshRate;
  pp.PresentationInterval = GetConfig().vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
  return pp;
}

// A Direct3D 9 device can only go fullscreen on the window it was created
// for (its focus window), and the device is made before the game has a
// window. So, the first time the game window is attached and before the
// game has made any textures, the device is made again on that window.
void SwitchToExclusive9(HWND hwnd) {
  Backend& b = B();
  if (g_exclusive || g_exclusive_tried) return;
  g_exclusive_tried = true;
  if (g_stats.tex_managed || g_stats.tex_rt || g_stats.depth) {
    Log("exclusive: textures already exist (%u managed, %u render targets, %u depth); staying borderless",
        g_stats.tex_managed, g_stats.tex_rt, g_stats.depth);
    return;
  }
  const D3DDISPLAYMODE mode = PickMode9();
  Log("exclusive: recreating the device on the game window %p, %ux%u@%u Hz, VSync %d", static_cast<void*>(hwnd),
      mode.Width, mode.Height, mode.RefreshRate, GetConfig().vsync ? 1 : 0);
  ReleaseChains();
  MsaaDeviceLost();
  MotionBlurDeviceLost();
  ReleaseGdi9();
  GpuTextDeviceLost();
  GpuTimerRelease9();
  EndScene9();
  for (auto& kv : g_depth_clear_rt) SafeRelease(kv.second);
  g_depth_clear_rt.clear();
  for (auto& kv : g_upload_tex) SafeRelease(kv.second);
  g_upload_tex.clear();
  for (auto& kv : g_staging) SafeRelease(kv.second);
  g_staging.clear();
  SafeRelease(g_white);
  const D3DPRESENT_PARAMETERS windowed = g_pp;
  const DWORD vp = g_behavior & (D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_SOFTWARE_VERTEXPROCESSING);
  ULONG left = b.dev9->Release();
  b.dev9 = nullptr;
  if (left) Log("exclusive: old device still has %lu references", left);
  g_pp = FullscreenParams9(hwnd, mode);
  if (CreateDevice9(vp, hwnd)) {
    g_exclusive = true;
    ResetState();
    Log("exclusive: fullscreen device made (behaviour %08lx)", g_behavior);
    return;
  }
  g_pp = windowed;
  if (!CreateDevice9(vp)) Log("exclusive: could not make the windowed device again either");
  else Log("exclusive: fullscreen failed; back to borderless");
  ResetState();
}

}  // namespace

bool AttachWindow9(HWND hwnd) {
  Backend& b = B();
  if (b.hwnd != hwnd) {
    ReleaseChains();
    b.hwnd = hwnd;
  }
  ApplyBorderless(hwnd);
  if (GetConfig().exclusive) SwitchToExclusive9(hwnd);
  if (!b.dev9) return false;
  if (g_lost && !TryRecover()) return true;
  return EnsureChains();
}

// ------------------------------------------------------------------- textures

bool CreateGpuTexture9(GpuTexture& t, UINT w, UINT h, UINT levels, bool render_target,
                       bool depth, bool alpha) {
  Backend& b = B();
  if (!b.dev9) return false;
  w = w ? w : 1;
  h = h ? h : 1;
  levels = levels ? levels : 1;
  HRESULT hr;
  if (depth) {
    hr = b.dev9->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &t.surf9, nullptr);
    t.fmt9 = D3DFMT_D24S8;
    if (FAILED(hr)) {
      hr = b.dev9->CreateDepthStencilSurface(w, h, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &t.surf9, nullptr);
      t.fmt9 = D3DFMT_D16;
    }
    if (FAILED(hr)) {
      ++g_stats.create_failed;
      Log("depth buffer %ux%u failed %08lx", w, h, hr);
      t.surf9 = nullptr;
      return false;
    }
    t.pool_default = true;
    t.depth = true;
    ++g_stats.depth;
  } else if (render_target) {
    t.fmt9 = alpha ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    hr = b.dev9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, static_cast<D3DFORMAT>(t.fmt9), D3DPOOL_DEFAULT, &t.tex9, nullptr);
    if (FAILED(hr)) {
      ++g_stats.create_failed;
      Log("render-target texture %ux%u %s failed %08lx", w, h, FormatName(static_cast<D3DFORMAT>(t.fmt9)), hr);
      t.tex9 = nullptr;
      return false;
    }
    t.tex9->GetSurfaceLevel(0, &t.surf9);
    t.pool_default = true;
    t.rt = true;
    // GDI can draw on X8R8G8B8 surfaces (IDirect3DSurface9::GetDC).
    t.gdi = !alpha;
    levels = 1;
    ++g_stats.tex_rt;
  } else {
    t.fmt9 = D3DFMT_A8R8G8B8;
    hr = b.dev9->CreateTexture(w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex9, nullptr);
    if (FAILED(hr)) {
      ++g_stats.create_failed;
      Log("texture %ux%u (%u levels) failed %08lx", w, h, levels, hr);
      t.tex9 = nullptr;
      return false;
    }
    levels = t.tex9->GetLevelCount();
    ++g_stats.tex_managed;
  }
  t.width = w;
  t.height = h;
  t.levels = levels;
  return true;
}

bool CreateTextureFromBGRA9(GpuTexture& t, const uint8_t* px, UINT w, UINT h) {
  Backend& b = B();
  if (!b.dev9) return false;
  HRESULT hr = b.dev9->CreateTexture(w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex9, nullptr);
  if (FAILED(hr)) {
    Log("replacement texture %ux%u failed %08lx", w, h, hr);
    t.tex9 = nullptr;
    return false;
  }
  t.fmt9 = D3DFMT_A8R8G8B8;
  t.width = w;
  t.height = h;
  t.levels = t.tex9->GetLevelCount();
  std::vector<uint8_t> cur(px, px + static_cast<size_t>(w) * h * 4), next;
  UINT cw = w, ch = h;
  for (UINT level = 0; level < t.levels; ++level) {
    UploadLevel9(t, level, cur.data(), cw, ch);
    UINT nw = 0, nh = 0;
    BoxDown(cur, cw, ch, next, nw, nh);
    cur.swap(next);
    cw = nw;
    ch = nh;
  }
  return true;
}

void UploadLevel9(GpuTexture& t, UINT level, const uint8_t* bgra, UINT w, UINT h) {
  if (!t.tex9 || level >= t.levels) return;
  ++g_stats.uploads;
  g_stats.upload_bytes += w * h * 4;
  if (!t.pool_default) {
    D3DLOCKED_RECT lr{};
    if (FAILED(t.tex9->LockRect(level, &lr, nullptr, 0))) {
      LogOnce("LockRect on a %ux%u texture failed", w, h);
      return;
    }
    for (UINT y = 0; y < h; ++y)
      std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                  bgra + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
    t.tex9->UnlockRect(level);
    return;
  }
  // Video memory: through a system-memory copy.
  IDirect3DSurface9* stage = Staging(w, h, static_cast<D3DFORMAT>(t.fmt9));
  IDirect3DSurface9* dst = nullptr;
  if (!stage || FAILED(t.tex9->GetSurfaceLevel(level, &dst))) return;
  D3DLOCKED_RECT lr{};
  if (SUCCEEDED(stage->LockRect(&lr, nullptr, 0))) {
    for (UINT y = 0; y < h; ++y)
      std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                  bgra + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
    stage->UnlockRect();
    EndScene9();
    HRESULT hr = B().dev9->UpdateSurface(stage, nullptr, dst, nullptr);
    if (FAILED(hr)) {
      // Fallback: a managed copy drawn into the target.
      LogOnce("UpdateSurface %ux%u failed %08lx; uploading render targets by drawing instead", w, h, hr);
      IDirect3DTexture9*& tmp = g_upload_tex[Key(w, h, D3DFMT_A8R8G8B8)];
      if (!tmp && FAILED(B().dev9->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tmp, nullptr)))
        tmp = nullptr;
      D3DLOCKED_RECT tl{};
      if (tmp && SUCCEEDED(tmp->LockRect(0, &tl, nullptr, 0))) {
        for (UINT y = 0; y < h; ++y)
          std::memcpy(static_cast<uint8_t*>(tl.pBits) + static_cast<size_t>(y) * tl.Pitch,
                      bgra + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
        tmp->UnlockRect(0);
        BlitParams p;
        p.dst9 = dst;
        p.dst_w = w;
        p.dst_h = h;
        p.dst_rect = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        p.src9 = tmp;
        p.src_w = w;
        p.src_h = h;
        p.src_rect = p.dst_rect;
        Blit9(p);
      }
    }
  }
  dst->Release();
}

bool DownloadTop9(GpuTexture& t, void (*row)(void*, UINT, const uint8_t*), void* ctx) {
  if (!t.tex9) return false;
  ++g_stats.readbacks;
  if (!t.pool_default) {
    D3DLOCKED_RECT lr{};
    if (FAILED(t.tex9->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return false;
    for (UINT y = 0; y < t.height; ++y)
      row(ctx, y, static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
    t.tex9->UnlockRect(0);
    return true;
  }
  if (!t.surf9) return false;
  IDirect3DSurface9* stage = Staging(t.width, t.height, static_cast<D3DFORMAT>(t.fmt9));
  if (!stage) return false;
  EndScene9();
  HRESULT hr = B().dev9->GetRenderTargetData(t.surf9, stage);
  if (FAILED(hr)) {
    LogOnce("GetRenderTargetData %ux%u failed %08lx", t.width, t.height, hr);
    return false;
  }
  D3DLOCKED_RECT lr{};
  if (FAILED(stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
  for (UINT y = 0; y < t.height; ++y)
    row(ctx, y, static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
  stage->UnlockRect();
  return true;
}

bool CopyTexture9(GpuTexture& dst, const GpuTexture& src) {
  if (!src.tex9) return false;
  if (!CreateGpuTexture9(dst, src.width, src.height, 1, true, false, src.fmt9 != D3DFMT_X8R8G8B8))
    return false;
  if (src.surf9 && src.rt) {
    EndScene9();
    HRESULT hr = B().dev9->StretchRect(src.surf9, nullptr, dst.surf9, nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) {
      LogOnce("StretchRect copy failed %08lx", hr);
      return false;
    }
    return true;
  }
  LogOnce("copy of a managed texture requested; not supported");
  return false;
}

void ClearDepth9(GpuTexture& t, float z) {
  Backend& b = B();
  if (!t.surf9 || !t.depth || g_lost) return;
  IDirect3DSurface9*& rt = g_depth_clear_rt[Key(t.width, t.height, D3DFMT_X8R8G8B8)];
  if (!rt && FAILED(b.dev9->CreateRenderTarget(t.width, t.height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                               FALSE, &rt, nullptr))) {
    rt = nullptr;
    LogOnce("depth clear target %ux%u failed", t.width, t.height);
    return;
  }
  SetTarget9(rt, t.surf9);
  b.dev9->Clear(0, nullptr, kClear9ZBuffer | (t.fmt9 == D3DFMT_D24S8 ? kClear9Stencil : 0), 0, z, 0);
}

// GDI on render targets. IDirect3DSurface9::GetDC does not work on
// render-target textures (drivers return D3DERR_INVALIDCALL), and reading
// the whole frame back for the game's few words of text halves the frame
// rate. Instead the game gets a DC on a DIB of the surface's size whose
// pixels are fetched from the GPU lazily: the GDI hooks (gdihook.cpp) say
// which rectangle each call can touch, only those 32x32 tiles are read back
// (through a small render target), and on ReleaseDC only the touched
// rectangles are copied back with UpdateSurface.
namespace {
constexpr UINT kGdiTile = 32;

struct GdiStats9 {
  unsigned sessions = 0, fetches = 0, uploads = 0;
  uint64_t fetch_px = 0, upload_px = 0;
} g_gdi;

IDirect3DSurface9* g_gdi_rt = nullptr;
UINT g_gdi_rt_w = 0, g_gdi_rt_h = 0;

struct LazyDC : GdiTarget {
  IDirect3DSurface9* surf = nullptr;  // not owned
  UINT w = 0, h = 0, tw = 0, th = 0;
  HDC dc = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ old = nullptr;
  uint8_t* bits = nullptr;
  std::vector<uint8_t> loaded;
  std::vector<uint8_t> dirty;  // tiles a call may have drawn on (always loaded)
  bool any_dirty = false;

  ~LazyDC() override {
    if (dc) {
      if (old) SelectObject(dc, old);
      DeleteDC(dc);
    }
    if (dib) DeleteObject(dib);
  }

  bool Create(UINT width, UINT height) {
    w = width;
    h = height;
    tw = (w + kGdiTile - 1) / kGdiTile;
    th = (h + kGdiTile - 1) / kGdiTile;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    void* p = nullptr;
    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    dc = CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!dib || !dc || !p) return false;
    bits = static_cast<uint8_t*>(p);
    old = SelectObject(dc, dib);
    loaded.assign(static_cast<size_t>(tw) * th, 0);
    dirty.assign(static_cast<size_t>(tw) * th, 0);
    return true;
  }

  void Begin() {
    std::fill(loaded.begin(), loaded.end(), 0);
    std::fill(dirty.begin(), dirty.end(), 0);
    any_dirty = false;
    // A fresh DC state for every session, as IDirect3DSurface9::GetDC gives.
    SelectClipRgn(dc, nullptr);
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, GetStockObject(SYSTEM_FONT));
    SelectObject(dc, GetStockObject(BLACK_PEN));
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    MoveToEx(dc, 0, 0, nullptr);
  }

  bool Fetch(const RECT& r) {
    const UINT rw = static_cast<UINT>(r.right - r.left), rh = static_cast<UINT>(r.bottom - r.top);
    if (!g_gdi_rt || g_gdi_rt_w < rw || g_gdi_rt_h < rh) {
      SafeRelease(g_gdi_rt);
      const UINT nw = std::max(g_gdi_rt_w, (rw + 127) & ~127u), nh = std::max(g_gdi_rt_h, (rh + 127) & ~127u);
      HRESULT hr = B().dev9->CreateRenderTarget(nw, nh, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_gdi_rt,
                                                nullptr);
      if (FAILED(hr)) {
        g_gdi_rt = nullptr;
        g_gdi_rt_w = g_gdi_rt_h = 0;
        LogOnce("GDI fetch target %ux%u failed %08lx", nw, nh, hr);
        return false;
      }
      g_gdi_rt_w = nw;
      g_gdi_rt_h = nh;
      if (GetConfig().d3d9_log) Log("GDI fetch target now %ux%u", nw, nh);
    }
    IDirect3DSurface9* stage = Staging(g_gdi_rt_w, g_gdi_rt_h, D3DFMT_X8R8G8B8);
    if (!stage) return false;
    EndScene9();
    RECT to{0, 0, static_cast<LONG>(rw), static_cast<LONG>(rh)};
    HRESULT hr = B().dev9->StretchRect(surf, &r, g_gdi_rt, &to, D3DTEXF_NONE);
    if (FAILED(hr)) {
      LogOnce("GDI fetch StretchRect failed %08lx", hr);
      return false;
    }
    hr = B().dev9->GetRenderTargetData(g_gdi_rt, stage);
    if (FAILED(hr)) {
      LogOnce("GDI fetch GetRenderTargetData failed %08lx", hr);
      return false;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    // Copy only tiles not loaded yet: loaded ones may already hold GDI output.
    for (UINT ty = static_cast<UINT>(r.top) / kGdiTile; ty * kGdiTile < static_cast<UINT>(r.bottom); ++ty) {
      for (UINT tx = static_cast<UINT>(r.left) / kGdiTile; tx * kGdiTile < static_cast<UINT>(r.right); ++tx) {
        uint8_t& done = loaded[static_cast<size_t>(ty) * tw + tx];
        if (done) continue;
        done = 1;
        const UINT x0 = tx * kGdiTile, y0 = ty * kGdiTile;
        const UINT x1 = std::min(x0 + kGdiTile, w), y1 = std::min(y0 + kGdiTile, h);
        for (UINT y = y0; y < y1; ++y)
          std::memcpy(bits + (static_cast<size_t>(y) * w + x0) * 4,
                      static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y - r.top) * lr.Pitch +
                          static_cast<size_t>(x0 - r.left) * 4,
                      (x1 - x0) * 4);
      }
    }
    stage->UnlockRect();
    ++g_gdi.fetches;
    g_gdi.fetch_px += static_cast<uint64_t>(rw) * rh;
    return true;
  }

  void Touch(const RECT& r) override {
    // Bounding box of the tiles r covers that are not loaded yet.
    UINT bx0 = tw, by0 = th, bx1 = 0, by1 = 0;
    for (UINT ty = static_cast<UINT>(r.top) / kGdiTile; ty * kGdiTile < static_cast<UINT>(r.bottom); ++ty)
      for (UINT tx = static_cast<UINT>(r.left) / kGdiTile; tx * kGdiTile < static_cast<UINT>(r.right); ++tx)
        if (!loaded[static_cast<size_t>(ty) * tw + tx]) {
          bx0 = std::min(bx0, tx);
          by0 = std::min(by0, ty);
          bx1 = std::max(bx1, tx + 1);
          by1 = std::max(by1, ty + 1);
        }
    if (bx0 < bx1) {
      RECT f{static_cast<LONG>(bx0 * kGdiTile), static_cast<LONG>(by0 * kGdiTile),
             static_cast<LONG>(std::min(bx1 * kGdiTile, w)), static_cast<LONG>(std::min(by1 * kGdiTile, h))};
      if (!Fetch(f)) {
        LogOnce("GDI fetch of %ld,%ld-%ld,%ld failed", f.left, f.top, f.right, f.bottom);
        return;  // never copy back pixels that were not fetched
      }
    }
    for (UINT ty = static_cast<UINT>(r.top) / kGdiTile; ty * kGdiTile < static_cast<UINT>(r.bottom); ++ty)
      for (UINT tx = static_cast<UINT>(r.left) / kGdiTile; tx * kGdiTile < static_cast<UINT>(r.right); ++tx)
        dirty[static_cast<size_t>(ty) * tw + tx] = 1;
    any_dirty = true;
  }

  void End() {
    GdiFlush();
    if (!any_dirty) return;
    IDirect3DSurface9* stage = Staging(w, h, D3DFMT_X8R8G8B8);
    if (!stage) return;
    EndScene9();
    // Copy back runs of touched tiles, one rectangle per run in a tile row.
    // Only touched tiles: they are the only ones known to be fetched.
    for (UINT ty = 0; ty < th; ++ty) {
      for (UINT tx = 0; tx < tw;) {
        if (!dirty[static_cast<size_t>(ty) * tw + tx]) {
          ++tx;
          continue;
        }
        UINT end = tx;
        while (end < tw && dirty[static_cast<size_t>(ty) * tw + end]) ++end;
        RECT r{static_cast<LONG>(tx * kGdiTile), static_cast<LONG>(ty * kGdiTile),
               static_cast<LONG>(std::min(end * kGdiTile, w)), static_cast<LONG>(std::min((ty + 1) * kGdiTile, h))};
        tx = end;
        D3DLOCKED_RECT lr{};
        if (FAILED(stage->LockRect(&lr, &r, 0))) continue;
        const UINT rw = static_cast<UINT>(r.right - r.left);
        for (LONG y = r.top; y < r.bottom; ++y)
          std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y - r.top) * lr.Pitch,
                      bits + (static_cast<size_t>(y) * w + static_cast<size_t>(r.left)) * 4, rw * 4);
        stage->UnlockRect();
        POINT pt{r.left, r.top};
        HRESULT hr = B().dev9->UpdateSurface(stage, &r, surf, &pt);
        if (FAILED(hr)) {
          LogOnce("GDI UpdateSurface failed %08lx", hr);
          continue;
        }
        ++g_gdi.uploads;
        g_gdi.upload_px += static_cast<uint64_t>(rw) * static_cast<UINT>(r.bottom - r.top);
      }
    }
  }
};

std::map<IDirect3DSurface9*, LazyDC*> g_lazy;

void ReleaseGdi9() {
  for (auto& kv : g_lazy) delete kv.second;
  g_lazy.clear();
  SafeRelease(g_gdi_rt);
  g_gdi_rt_w = g_gdi_rt_h = 0;
}
}  // namespace

bool GpuGetDC9(GpuTexture& t, HDC* dc) {
  if (!t.surf9 || !t.gdi || g_lost) return false;
  MsaaRelease(t.Native());
  LazyDC*& l = g_lazy[t.surf9];
  if (l && (l->w != t.width || l->h != t.height)) {
    delete l;
    l = nullptr;
  }
  if (!l) {
    l = new LazyDC();
    l->surf = t.surf9;
    if (!l->Create(t.width, t.height)) {
      Log("GDI surface %ux%u could not be made; GDI uses the CPU copy for this surface", t.width, t.height);
      delete l;
      l = nullptr;
      t.gdi = false;
      ++g_stats.gdi_fail;
      return false;
    }
  }
  l->Begin();
  GdiTrack(l->dc, l, t.width, t.height, "d3d9 render target");
  *dc = l->dc;
  ++g_stats.gdi_gpu;
  ++g_gdi.sessions;
  LogOnce("GDI on render targets: fetched from the GPU only where the game draws");
  return true;
}

void GpuReleaseDC9(GpuTexture& t, HDC dc) {
  auto it = g_lazy.find(t.surf9);
  if (it == g_lazy.end() || !it->second || it->second->dc != dc) return;
  GdiUntrack(dc);
  it->second->End();
}

// ---------------------------------------------------------------------- scene

void BeginScene9() {
  if (g_in_scene || !B().dev9) return;
  HRESULT hr = B().dev9->BeginScene();
  if (FAILED(hr)) {
    LogOnce("BeginScene failed %08lx", hr);
    return;
  }
  g_in_scene = true;
}

void EndScene9() {
  if (!g_in_scene) return;
  g_in_scene = false;
  if (B().dev9) B().dev9->EndScene();
}

// ----------------------------------------------------------------------- blit

namespace {

// Everything of a blit but the vertices and the draw; returns the vertex colour.
DWORD SetupBlit9(const BlitParams& p) {
  Backend& b = B();
  IDirect3DDevice9* d = b.dev9;
  SetTarget9(p.dst9, nullptr);
  SetViewport9(D3DVIEWPORT9{0, 0, p.dst_w, p.dst_h, 0.0f, 1.0f});
  RS9(D3DRS_ZENABLE, FALSE);
  RS9(D3DRS_ZWRITEENABLE, FALSE);
  RS9(D3DRS_ALPHABLENDENABLE, p.blend || p.premul || p.erase ? TRUE : FALSE);
  if (p.erase) {
    RS9(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    RS9(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  } else if (p.premul) {
    RS9(D3DRS_SRCBLEND, D3DBLEND_ONE);
    RS9(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  } else if (p.blend) {
    RS9(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    RS9(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  }
  RS9(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  RS9(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  RS9(D3DRS_ALPHATESTENABLE, p.alpha_key ? TRUE : FALSE);
  RS9(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
  RS9(D3DRS_ALPHAREF, 0);
  RS9(D3DRS_CULLMODE, D3DCULL_NONE);
  RS9(D3DRS_FOGENABLE, FALSE);
  RS9(D3DRS_LIGHTING, FALSE);
  RS9(D3DRS_SPECULARENABLE, FALSE);
  RS9(D3DRS_FILLMODE, D3DFILL_SOLID);
  RS9(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  RS9(D3DRS_STENCILENABLE, FALSE);
  RS9(D3DRS_SCISSORTESTENABLE, FALSE);
  RS9(D3DRS_CLIPPING, TRUE);
  RS9(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  RS9(D3DRS_COLORWRITEENABLE, p.alpha_only ? D3DCOLORWRITEENABLE_ALPHA : 0xf);
  RS9(D3DRS_DEPTHBIAS, 0);
  RS9(D3DRS_SLOPESCALEDEPTHBIAS, 0);
  if (p.src9) {
    Tex9(0, p.src9);
    const DWORD op = p.tint ? D3DTOP_MODULATE : D3DTOP_SELECTARG1;
    TSS9(0, D3DTSS_COLOROP, op);
    TSS9(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    TSS9(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    TSS9(0, D3DTSS_ALPHAOP, op);
    TSS9(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    TSS9(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    const DWORD f = p.linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    SS9(0, D3DSAMP_MAGFILTER, f);
    SS9(0, D3DSAMP_MINFILTER, f);
    SS9(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    SS9(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    SS9(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    SS9(0, D3DSAMP_MAXMIPLEVEL, 0);
    SS9(0, D3DSAMP_MAXANISOTROPY, 1);
    SS9(0, D3DSAMP_MIPMAPLODBIAS, 0);
  } else {
    Tex9(0, nullptr);
    TSS9(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    TSS9(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    TSS9(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    TSS9(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  }
  TSS9(0, D3DTSS_TEXCOORDINDEX, 0);
  TSS9(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  TSS9(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  TSS9(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  auto to8 = [](float v) {
    return static_cast<DWORD>(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
  };
  d->SetFVF(kFvf9XYZRHW | kFvf9Diffuse | kFvf9Tex1);
  return p.src9 && !p.tint ? 0xffffffff
                           : (to8(p.fill[3]) << 24) | (to8(p.fill[0]) << 16) | (to8(p.fill[1]) << 8) | to8(p.fill[2]);
}

struct BlitV9 {
  float x, y, z, rhw;
  DWORD c;
  float u, v;
};

}  // namespace

void Blit9(const BlitParams& p) {
  Backend& b = B();
  if (!b.dev9 || !p.dst9 || !p.dst_w || !p.dst_h || g_lost) return;
  ++g_stats.blits;
  IDirect3DDevice9* d = b.dev9;
  const DWORD color = SetupBlit9(p);
  using V = BlitV9;
  // Direct3D 9 pixel centres are at integer coordinates: shift by half a
  // pixel so texels land on pixels exactly.
  const float x0 = static_cast<float>(p.dst_rect.left) - 0.5f, x1 = static_cast<float>(p.dst_rect.right) - 0.5f;
  const float y0 = static_cast<float>(p.dst_rect.top) - 0.5f, y1 = static_cast<float>(p.dst_rect.bottom) - 0.5f;
  const float u0 = static_cast<float>(p.src_rect.left) / p.src_w, u1 = static_cast<float>(p.src_rect.right) / p.src_w;
  const float v0 = static_cast<float>(p.src_rect.top) / p.src_h, v1 = static_cast<float>(p.src_rect.bottom) / p.src_h;
  const V v[4] = {{x0, y0, 0, 1, color, u0, v0},
                  {x1, y0, 0, 1, color, u1, v0},
                  {x0, y1, 0, 1, color, u0, v1},
                  {x1, y1, 0, 1, color, u1, v1}};
  BeginScene9();
  HRESULT hr = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
  if (FAILED(hr)) LogOnce("blit draw failed %08lx", hr);
}

void BlitQuads9(const BlitParams& p, const BlitQuad* q, size_t n) {
  Backend& b = B();
  if (!b.dev9 || !p.dst9 || !p.dst_w || !p.dst_h || g_lost || !n) return;
  ++g_stats.blits;
  IDirect3DDevice9* d = b.dev9;
  const DWORD color = SetupBlit9(p);
  static std::vector<BlitV9> v;
  const float ku = 1.0f / p.src_w, kv = 1.0f / p.src_h;
  BeginScene9();
  for (size_t done = 0; done < n;) {
    const size_t m = std::min<size_t>(n - done, kQuadBatch);
    v.resize(m * 6);
    for (size_t i = 0; i < m; ++i) {
      const BlitQuad& e = q[done + i];
      const float x0 = e.dst.left - 0.5f, x1 = e.dst.right - 0.5f;
      const float y0 = e.dst.top - 0.5f, y1 = e.dst.bottom - 0.5f;
      const float u0 = e.src.left * ku, u1 = e.src.right * ku, v0 = e.src.top * kv, v1 = e.src.bottom * kv;
      BlitV9* o = &v[i * 6];
      o[0] = {x0, y0, 0, 1, color, u0, v0};
      o[1] = {x1, y0, 0, 1, color, u1, v0};
      o[2] = {x0, y1, 0, 1, color, u0, v1};
      o[3] = o[2];
      o[4] = o[1];
      o[5] = {x1, y1, 0, 1, color, u1, v1};
    }
    HRESULT hr = d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(m * 2), v.data(), sizeof(BlitV9));
    if (FAILED(hr)) LogOnce("batched blit draw failed %08lx", hr);
    done += m;
  }
}

// MotionBlur on Direct3D 9 (dd/motionblur.cpp): the scene copy in p.src9 is
// drawn again `layers - 1` times over p.dst9, each time zoomed a little more
// towards the focus point (fu, fv in 0..1), blended so that the result is the
// average of all the layers. A grid carries the zoom per vertex, so the
// middle of the screen (inside `inner`, ramping to full at `outer`, measured
// from the screen centre in half-heights) stays sharp, as in the D3D11 shader.
void ZoomBlur9(const BlitParams& p_in, float fu, float fv, float len, float inner, float outer,
               int layers) {
  Backend& b = B();
  if (!b.dev9 || !p_in.dst9 || !p_in.src9 || !p_in.dst_w || !p_in.dst_h || g_lost || layers < 2) return;
  BlitParams p = p_in;
  p.blend = true;
  p.tint = true;  // texture x vertex colour: the vertex alpha sets each layer's weight
  p.linear = true;
  SetupBlit9(p);
  constexpr int GX = 32, GY = 18;
  static std::vector<BlitV9> v;
  static std::vector<WORD> idx;
  static std::vector<float> zoom;  // per grid vertex: how far it moves towards the focus at full length
  v.resize((GX + 1) * (GY + 1));
  zoom.resize(v.size());
  if (idx.empty()) {
    for (int y = 0; y < GY; ++y)
      for (int x = 0; x < GX; ++x) {
        const WORD a = static_cast<WORD>(y * (GX + 1) + x), c = static_cast<WORD>(a + GX + 1);
        const WORD q[6] = {a, static_cast<WORD>(a + 1), c, c, static_cast<WORD>(a + 1), static_cast<WORD>(c + 1)};
        idx.insert(idx.end(), q, q + 6);
      }
  }
  const float aspect = static_cast<float>(p.dst_w) / p.dst_h;
  for (int y = 0; y <= GY; ++y)
    for (int x = 0; x <= GX; ++x) {
      const float u = static_cast<float>(x) / GX, vv = static_cast<float>(y) / GY;
      const float dx = (u - 0.5f) * 2.0f * aspect, dy = (vv - 0.5f) * 2.0f;
      const float r = std::sqrt(dx * dx + dy * dy);
      float t = outer > inner ? (r - inner) / (outer - inner) : (r >= inner ? 1.0f : 0.0f);
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      zoom[y * (GX + 1) + x] = len * t * t * (3.0f - 2.0f * t);
    }
  BeginScene9();
  for (int k = 1; k < layers; ++k) {
    const float step = static_cast<float>(k) / (layers - 1);
    // Layer k joins the k layers already there with weight 1/(k+1).
    const DWORD a = static_cast<DWORD>(255.0f / (k + 1) + 0.5f);
    const DWORD color = (a << 24) | 0xffffff;
    for (int y = 0; y <= GY; ++y)
      for (int x = 0; x <= GX; ++x) {
        const int i = y * (GX + 1) + x;
        const float u = static_cast<float>(x) / GX, vv = static_cast<float>(y) / GY;
        const float s = 1.0f - zoom[i] * step;
        v[i] = {u * p.dst_w - 0.5f, vv * p.dst_h - 0.5f, 0, 1, color, fu + (u - fu) * s, fv + (vv - fv) * s};
      }
    HRESULT hr = b.dev9->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(v.size()),
                                                static_cast<UINT>(idx.size() / 3), idx.data(), D3DFMT_INDEX16,
                                                v.data(), sizeof(BlitV9));
    if (FAILED(hr)) {
      LogOnce("motion blur layer draw failed %08lx", hr);
      break;
    }
  }
}

// -------------------------------------------------------------------- present

namespace {
// GPU time per frame (Profile=1), as in the D3D11 renderer.
struct GpuTimer9 {
  IDirect3DQuery9* disjoint = nullptr;
  IDirect3DQuery9* freq = nullptr;
  IDirect3DQuery9* begin = nullptr;
  IDirect3DQuery9* end = nullptr;
  bool pending = false;
};
GpuTimer9 g_gpu9[6];
unsigned g_gpu9_i = 0;
bool g_gpu9_open = false;
bool g_gpu9_begun = false;
bool g_gpu9_broken = false;

void GpuTimerRelease9() {
  for (GpuTimer9& t : g_gpu9) {
    SafeRelease(t.disjoint);
    SafeRelease(t.freq);
    SafeRelease(t.begin);
    SafeRelease(t.end);
    t.pending = false;
  }
  g_gpu9_open = false;
}

void GpuTimerFrame9() {
  if (!prof::On() || g_gpu9_broken) return;
  IDirect3DDevice9* dev = B().dev9;
  if (g_gpu9_open) {
    GpuTimer9& t = g_gpu9[g_gpu9_i % 6];
    if (!g_gpu9_begun) t.begin->Issue(D3DISSUE_END);
    t.end->Issue(D3DISSUE_END);
    t.freq->Issue(D3DISSUE_END);
    t.disjoint->Issue(D3DISSUE_END);
    t.pending = true;
    g_gpu9_open = false;
    ++g_gpu9_i;
  }
  for (GpuTimer9& t : g_gpu9) {
    if (!t.pending) continue;
    BOOL dj = FALSE;
    UINT64 f = 0, t0 = 0, t1 = 0;
    if (t.disjoint->GetData(&dj, sizeof(dj), 0) != S_OK) continue;
    if (t.freq->GetData(&f, sizeof(f), 0) != S_OK) continue;
    if (t.begin->GetData(&t0, sizeof(t0), 0) != S_OK) continue;
    if (t.end->GetData(&t1, sizeof(t1), 0) != S_OK) continue;
    t.pending = false;
    if (!dj && f && t1 >= t0) prof::GpuFrameTime(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(f));
  }
  GpuTimer9& n = g_gpu9[g_gpu9_i % 6];
  if (n.pending) return;
  if (!n.disjoint) {
    if (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &n.disjoint)) ||
        FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &n.freq)) ||
        FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &n.begin)) ||
        FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &n.end))) {
      Log("perf: Direct3D 9 timestamp queries not available");
      GpuTimerRelease9();
      g_gpu9_broken = true;
      return;
    }
  }
  n.disjoint->Issue(D3DISSUE_BEGIN);
  g_gpu9_open = true;
  g_gpu9_begun = false;  // the begin stamp goes in at the frame's first draw
}
}  // namespace

void GpuTimerMark9() {
  if (!g_gpu9_open || g_gpu9_begun) return;
  g_gpu9_begun = true;
  g_gpu9[g_gpu9_i % 6].begin->Issue(D3DISSUE_END);
}


namespace {

// ---- VSync without a desktop compositor ------------------------------------
// With the DWM (Vista/7 Aero, 8+), presenting on the D3DPRESENT_INTERVAL_ONE
// chain is enough. Without it (Windows XP, Vista/7 basic theme) a windowed
// Present is a plain blit to the desktop: XP drivers often ignore the
// interval (the frame rate is not capped) or blit at any scan line (tearing).
// There mcm2fix waits for the vertical blank itself and presents on the
// immediate chain right as it starts; if the driver cannot report the scan
// line, frames are paced to the refresh rate with the precise timer.
enum class VsyncMode { kSwapChain, kRasterPoll, kTimer };

struct ManualVsync {
  bool decided = false;
  VsyncMode mode = VsyncMode::kSwapChain;
  LARGE_INTEGER freq{}, next{};
  double period = 0;  // QPC ticks per refresh
  unsigned waits = 0, timeouts = 0, frames = 0;
  double wait_ms = 0, flush_ms = 0;
  DWORD last_log = 0;
} g_mv;

bool CompositorOn() {
  OSVERSIONINFOA v{};
  v.dwOSVersionInfoSize = sizeof(v);
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  GetVersionExA(&v);
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
  if (v.dwMajorVersion < 6) return false;  // XP / Server 2003: never
  if (v.dwMajorVersion > 6 || v.dwMinorVersion >= 2) return true;  // 8+: always
  HMODULE dwm = LoadLibraryA("dwmapi.dll");
  using Fn = HRESULT(WINAPI*)(BOOL*);
  auto fn = dwm ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmIsCompositionEnabled")))
                : nullptr;
  BOOL on = FALSE;
  return fn && SUCCEEDED(fn(&on)) && on;
}

void DecideVsync(IDirect3DSwapChain9* sc) {
  if (g_mv.decided) return;
  g_mv.decided = true;
  QueryPerformanceFrequency(&g_mv.freq);
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  DWORD hz = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
  if (hz <= 1 || hz > 500) hz = 60;  // 0/1 = "hardware default"
  g_mv.period = static_cast<double>(g_mv.freq.QuadPart) / hz;
  const bool comp = CompositorOn();
  const bool scanline = (g_caps.Caps & D3DCAPS_READ_SCANLINE) != 0;
  D3DRASTER_STATUS rs{};
  const bool raster_ok = scanline && sc && SUCCEEDED(sc->GetRasterStatus(&rs));
  if (comp || g_exclusive) g_mv.mode = VsyncMode::kSwapChain;
  else if (raster_ok) g_mv.mode = VsyncMode::kRasterPoll;
  else g_mv.mode = VsyncMode::kTimer;
  if (g_exclusive) Log("vsync: exclusive fullscreen, the swap chain waits for the refresh itself");
  Log("vsync: compositor %s, refresh %lu Hz, READ_SCANLINE cap %d, GetRasterStatus %s (line %u) -> %s",
      comp ? "on" : "off", hz, scanline ? 1 : 0, raster_ok ? "ok" : "failed", rs.ScanLine,
      g_mv.mode == VsyncMode::kSwapChain ? "swap chain interval ONE"
      : g_mv.mode == VsyncMode::kRasterPoll ? "own wait for vertical blank, then immediate present"
                                            : "paced to the refresh rate by timer, then immediate present");
}

// Waits until the vertical blank starts (or, by timer, until the next
// refresh is due). Returns false when it gave up.
bool WaitForBlank(IDirect3DSwapChain9* sc) {
  LARGE_INTEGER t0, t;
  // Let the GPU finish the frame first, so the blit queued by Present runs
  // straight away instead of whenever the GPU gets to it.
  QueryPerformanceCounter(&t0);
  if (!g_vsync_done) B().dev9->CreateQuery(D3DQUERYTYPE_EVENT, &g_vsync_done);
  if (g_vsync_done && SUCCEEDED(g_vsync_done->Issue(D3DISSUE_END))) {
    for (int i = 0; i < 100000; ++i) {
      HRESULT q = g_vsync_done->GetData(nullptr, 0, D3DGETDATA_FLUSH);
      if (q != S_FALSE) break;
      QueryPerformanceCounter(&t);
      if (t.QuadPart - t0.QuadPart > g_mv.freq.QuadPart / 20) break;  // 50 ms
      YieldProcessor();
    }
  }
  QueryPerformanceCounter(&t);
  g_mv.flush_ms += static_cast<double>(t.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(g_mv.freq.QuadPart);
  t0 = t;
  bool ok = true;
  if (g_mv.mode == VsyncMode::kRasterPoll) {
    D3DRASTER_STATUS rs{};
    const LONGLONG limit = static_cast<LONGLONG>(g_mv.period * 2);
    // If already in the blank, this frame took more than a refresh: go now.
    if (SUCCEEDED(sc->GetRasterStatus(&rs)) && !rs.InVBlank) {
      for (;;) {
        if (FAILED(sc->GetRasterStatus(&rs))) { ok = false; break; }
        if (rs.InVBlank) break;
        QueryPerformanceCounter(&t);
        if (t.QuadPart - t0.QuadPart > limit) { ok = false; break; }
        // Sleep while the beam is far from the bottom, spin near it.
        const double left = static_cast<double>(g_mv.period) *
                            (1.0 - static_cast<double>(rs.ScanLine) / static_cast<double>(g_sc.h ? g_sc.h : 600));
        if (left > g_mv.freq.QuadPart / 500.0) Sleep(1);
        else YieldProcessor();
      }
    }
  } else {
    const LONGLONG period = static_cast<LONGLONG>(g_mv.period);
    if (g_mv.next.QuadPart == 0 || t0.QuadPart - g_mv.next.QuadPart > period) g_mv.next.QuadPart = t0.QuadPart;
    for (;;) {
      QueryPerformanceCounter(&t);
      const LONGLONG left = g_mv.next.QuadPart - t.QuadPart;
      if (left <= 0) break;
      if (left > g_mv.freq.QuadPart / 500) Sleep(1);
      else YieldProcessor();
    }
    g_mv.next.QuadPart += period;
  }
  QueryPerformanceCounter(&t);
  ++g_mv.waits;
  if (!ok) ++g_mv.timeouts;
  g_mv.wait_ms += static_cast<double>(t.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(g_mv.freq.QuadPart);
  return ok;
}

void LogVsync() {
  ++g_mv.frames;
  const DWORD now = GetTickCount();
  if (!g_mv.last_log) g_mv.last_log = now;
  if (now - g_mv.last_log < 5000) return;
  const double secs = (now - g_mv.last_log) / 1000.0;
  // Every 5 s for the first minute, then every minute.
  static unsigned logs = 0;
  if (logs >= 12 && now - g_mv.last_log < 60000) return;
  ++logs;
  Log("vsync %.0fs: %.1f fps, own waits %u (GPU finish %.2f ms + blank wait %.2f ms avg, %u gave up)", secs,
      g_mv.frames / secs, g_mv.waits, g_mv.waits ? g_mv.flush_ms / g_mv.waits : 0.0,
      g_mv.waits ? g_mv.wait_ms / g_mv.waits : 0.0, g_mv.timeouts);
  g_mv.last_log = now;
  g_mv.frames = g_mv.waits = g_mv.timeouts = 0;
  g_mv.wait_ms = g_mv.flush_ms = 0;
}

}  // namespace

void Present9(const GpuTexture& src, UINT src_w, UINT src_h, UINT interval) {
  Backend& b = B();
  ++Counters().presents;
  ++g_stats.presents;
  ReportStats();
  if (!b.dev9 || !b.hwnd || IsIconic(b.hwnd)) return;
  if (g_lost) GpuTimerRelease9();
  if (g_lost && !TryRecover()) return;
  prof::Scope zone(prof::kCompose);
  KeepBorderless();
  if (!EnsureChains()) return;
  if (interval) DecideVsync(g_sc.vsync);
  const bool own_wait = interval && g_mv.mode != VsyncMode::kSwapChain;
  IDirect3DSwapChain9* sc = interval && !own_wait ? g_sc.vsync : g_sc.now;
  IDirect3DSurface9* back = nullptr;
  if (FAILED(sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
  EndScene9();
  b.dev9->ColorFill(back, nullptr, D3DCOLOR_XRGB(0, 0, 0));
  if (src.tex9 && src_w && src_h) {
    // Fit inside the window, keeping the game's aspect ratio.
    double sx = static_cast<double>(g_sc.w) / src_w, sy = static_cast<double>(g_sc.h) / src_h;
    double s = sx < sy ? sx : sy;
    LONG w = static_cast<LONG>(src_w * s + 0.5), h = static_cast<LONG>(src_h * s + 0.5);
    BlitParams p;
    p.dst9 = back;
    p.dst_w = g_sc.w;
    p.dst_h = g_sc.h;
    p.dst_rect = {static_cast<LONG>(g_sc.w - w) / 2, static_cast<LONG>(g_sc.h - h) / 2, 0, 0};
    p.dst_rect.right = p.dst_rect.left + w;
    p.dst_rect.bottom = p.dst_rect.top + h;
    p.src9 = src.tex9;
    p.src_w = src.width;
    p.src_h = src.height;
    p.src_rect = {0, 0, static_cast<LONG>(src_w), static_cast<LONG>(src_h)};
    p.linear = true;
    Blit9(p);
    GpuTextComposite(src, nullptr, back, g_sc.w, g_sc.h, p.dst_rect);
  }
  DrawRendererLabel(nullptr, back, g_sc.w, g_sc.h);
  back->Release();
  EndScene9();
  PerfBeforePresent();
  zone.Switch(prof::kSwapPresent);
  if (own_wait) WaitForBlank(g_sc.vsync);
  HRESULT hr = sc->Present(nullptr, nullptr, nullptr, nullptr, 0);
  if (interval) LogVsync();
  zone.Switch(prof::kCompose);
  if (!g_lost && hr != D3DERR_DEVICELOST) GpuTimerFrame9();
  if (hr == D3DERR_DEVICELOST) {
    if (!g_lost) Log("device lost (Present)");
    g_lost = true;
  } else if (FAILED(hr)) {
    LogOnce("Present failed %08lx", hr);
  }
  // A summary every 5 seconds for the first minute (D3D9Log).
  if (GetConfig().d3d9_log) {
    static DWORD start = GetTickCount(), last = start;
    const DWORD now = GetTickCount();
    if (now - last >= 5000 && now - start <= 65000) {
      last = now;
      Log("5s: %u presents, %u blits, %u uploads (%u KB), %u readbacks, GDI on GPU %u (failed %u); "
          "textures so far: %u managed, %u render targets, %u depth, %u failed",
          g_stats.presents, g_stats.blits, g_stats.uploads, g_stats.upload_bytes >> 10, g_stats.readbacks,
          g_stats.gdi_gpu, g_stats.gdi_fail, g_stats.tex_managed, g_stats.tex_rt, g_stats.depth,
          g_stats.create_failed);
      g_stats.presents = g_stats.blits = g_stats.uploads = g_stats.upload_bytes = g_stats.readbacks = 0;
      g_stats.gdi_gpu = g_stats.gdi_fail = 0;
    }
    static DWORD gdi_last = now;
    if (now - gdi_last >= 5000) {
      gdi_last = now;
      if (g_gdi.sessions)
        Log("GDI 5s: %u DCs, %u fetches (%llu px), %u uploads (%llu px)", g_gdi.sessions, g_gdi.fetches,
            static_cast<unsigned long long>(g_gdi.fetch_px), g_gdi.uploads,
            static_cast<unsigned long long>(g_gdi.upload_px));
      g_gdi = GdiStats9{};
    }
  }
}

void WaitVBlank9() {
  IDirect3DSwapChain9* sc = nullptr;
  {
    DD_LOCK;
    sc = g_sc.vsync;
    if (sc) sc->AddRef();
  }
  if (!sc) {
    Sleep(1);
    return;
  }
  // Poll the scan line until it wraps or the blank starts.
  D3DRASTER_STATUS rs{};
  if (FAILED(sc->GetRasterStatus(&rs))) {
    sc->Release();
    Sleep(1);
    return;
  }
  const UINT start = rs.ScanLine;
  const DWORD t0 = GetTickCount();
  for (;;) {
    if (FAILED(sc->GetRasterStatus(&rs))) break;
    if (rs.InVBlank || rs.ScanLine < start) break;
    if (GetTickCount() - t0 > 40) break;
    Sleep(0);
  }
  sc->Release();
}

}  // namespace dd
