#include "directdraw.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include <algorithm>

#include "backend.h"
#include "device.h"
#include "surface.h"

namespace crash { void OnModeChange(DWORD w, DWORD h); }

namespace dd {

using d3d7::D3D_OK;
using d3d7::D3DENUMRET_CANCEL;

namespace {

DDPIXELFORMAT FormatFor(UINT bpp) {
  if (bpp == 32) return MakeRGBFormat(32, 0xff0000, 0xff00, 0xff, 0);
  if (bpp == 8) {
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    pf.dwRGBBitCount = 8;
    return pf;
  }
  return MakeRGBFormat(16, 0xf800, 0x07e0, 0x001f, 0);
}

void FillModeDesc(DDSURFACEDESC2& d, UINT w, UINT h, UINT bpp, UINT hz) {
  std::memset(&d, 0, sizeof(d));
  d.dwSize = sizeof(d);
  d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
  d.dwWidth = w;
  d.dwHeight = h;
  d.lPitch = static_cast<LONG>(w * bpp / 8);
  d.dwRefreshRate = hz;
  d.ddpfPixelFormat = FormatFor(bpp);
}

void FillCaps(DDCAPS& c) {
  DWORD size = c.dwSize ? c.dwSize : sizeof(DDCAPS);
  if (size > sizeof(DDCAPS)) size = sizeof(DDCAPS);
  std::memset(&c, 0, size);
  c.dwSize = size;
  c.dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL |
             DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_CANBLTSYSMEM | DDCAPS_CANCLIP |
             DDCAPS_CANCLIPSTRETCHED | DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_ZBLTS;
  c.dwCaps2 = DDCAPS2_CANRENDERWINDOWED | DDCAPS2_WIDESURFACES | DDCAPS2_PRIMARYGAMMA |
              DDCAPS2_FLIPNOVSYNC | DDCAPS2_NOPAGELOCKREQUIRED;
  c.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
  c.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
  c.dwVidMemTotal = 256u << 20;
  c.dwVidMemFree = 256u << 20;
  c.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP |
                     DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE |
                     DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_TEXTURE |
                     DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP |
                     DDSCAPS_LOCALVIDMEM;
  c.dwSVBCaps = c.dwCaps;
  c.dwVSBCaps = c.dwCaps;
  c.dwSSBCaps = c.dwCaps;
}

}  // namespace

// ----------------------------------------------------------------- DirectDraw

namespace {
DirectDraw* g_current = nullptr;
}

DirectDraw* DirectDraw::Current() { return g_current; }

DirectDraw::DirectDraw() : d3d_(this) {
  g_current = this;
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
    mode_w_ = dm.dmPelsWidth;
    mode_h_ = dm.dmPelsHeight;
  }
  mode_bpp_ = 32;
  mode_format_ = FormatFor(mode_bpp_);
}

DirectDraw::~DirectDraw() {
  if (g_current == this) g_current = nullptr;
  Log("DirectDraw released");
}

HRESULT DirectDraw::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (riid == d3d7::IID_IDirectDraw7 || riid == d3d7::IID_IUnknown_) {
    AddRef();
    *out = static_cast<IDirectDraw7*>(this);
    return S_OK;
  }
  if (riid == d3d7::IID_IDirect3D7) {
    AddRef();
    *out = static_cast<d3d7::IDirect3D7*>(&d3d_);
    return S_OK;
  }
  LogOnce("DirectDraw::QueryInterface for unsupported {%08lx-%04x-%04x}", riid.Data1,
          riid.Data2, riid.Data3);
  return E_NOINTERFACE;
}

ULONG DirectDraw::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG DirectDraw::Release() {
  LONG n = InterlockedDecrement(&refs_);
  if (!n) {
    DD_LOCK;
    delete this;
  }
  return static_cast<ULONG>(n);
}

HRESULT DirectDraw::Compact() { return DD_OK; }

HRESULT DirectDraw::CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = new Clipper();
  return DD_OK;
}

HRESULT DirectDraw::CreatePalette(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out,
                                  IUnknown*) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = new Palette(flags, entries);
  return DD_OK;
}

HRESULT DirectDraw::CreateSurface(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out, IUnknown*) {
  DD_LOCK;
  if (!desc || !out) return DDERR_INVALIDPARAMS;
  *out = nullptr;
  InitDevice();
  Surface* s = nullptr;
  HRESULT hr = Surface::Create(this, *desc, &s);
  if (FAILED(hr)) return hr;
  *out = s;
  return DD_OK;
}

HRESULT DirectDraw::DuplicateSurface(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7*) {
  LogOnce("DuplicateSurface unsupported");
  return DDERR_UNSUPPORTED;
}

HRESULT DirectDraw::EnumDisplayModes(DWORD, LPDDSURFACEDESC2 filter, LPVOID ctx,
                                     LPDDENUMMODESCALLBACK2 cb) {
  if (!cb) return DDERR_INVALIDPARAMS;
  // Display modes are virtual (the window stays at desktop size), so offer
  // the classic 4:3 sizes plus widescreen sizes up to the desktop's.
  std::vector<std::pair<UINT, UINT>> modes = {{640, 480},  {800, 600},   {1024, 768},
                                              {1152, 864}, {1280, 960},  {1280, 1024},
                                              {1600, 1200}};
  static const UINT wide[][2] = {{1280, 720},  {1280, 800},  {1366, 768},  {1440, 900},
                                 {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200},
                                 {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440},
                                 {3840, 1600}, {3840, 2160}};
  const UINT desk_w = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
  const UINT desk_h = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
  for (const auto& m : wide)
    if (m[0] <= desk_w && m[1] <= desk_h) modes.push_back({m[0], m[1]});
  if (desk_w >= 640 && desk_h >= 480) modes.push_back({desk_w, desk_h});
  std::sort(modes.begin(), modes.end());
  modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
  static const UINT depths[] = {16};
  for (UINT bpp : depths) {
    for (const auto& mode : modes) {
      const UINT m[2] = {mode.first, mode.second};
      if (filter && (filter->dwFlags & DDSD_WIDTH) && filter->dwWidth != m[0]) continue;
      if (filter && (filter->dwFlags & DDSD_HEIGHT) && filter->dwHeight != m[1]) continue;
      if (filter && (filter->dwFlags & DDSD_PIXELFORMAT) &&
          filter->ddpfPixelFormat.dwRGBBitCount != bpp)
        continue;
      DDSURFACEDESC2 d;
      FillModeDesc(d, m[0], m[1], bpp, 60);
      if (cb(&d, ctx) == DDENUMRET_CANCEL) return DD_OK;
    }
  }
  return DD_OK;
}

HRESULT DirectDraw::EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) {
  LogOnce("EnumSurfaces unsupported");
  return DDERR_UNSUPPORTED;
}

HRESULT DirectDraw::FlipToGDISurface() { return DD_OK; }

HRESULT DirectDraw::GetCaps(LPDDCAPS drv, LPDDCAPS hel) {
  if (drv) FillCaps(*drv);
  if (hel) FillCaps(*hel);
  return DD_OK;
}

HRESULT DirectDraw::GetDisplayMode(LPDDSURFACEDESC2 desc) {
  if (!desc) return DDERR_INVALIDPARAMS;
  DDSURFACEDESC2 d;
  FillModeDesc(d, mode_w_, mode_h_, mode_bpp_, 60);
  DWORD size = desc->dwSize == sizeof(DDSURFACEDESC) ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2);
  std::memcpy(desc, &d, size);
  desc->dwSize = size;
  return DD_OK;
}

HRESULT DirectDraw::GetFourCCCodes(LPDWORD n, LPDWORD) {
  if (n) *n = 0;
  return DD_OK;
}

HRESULT DirectDraw::GetGDISurface(LPDIRECTDRAWSURFACE7* out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = primary_;
  if (!primary_) return DDERR_NOTFOUND;
  primary_->AddRef();
  return DD_OK;
}

HRESULT DirectDraw::GetMonitorFrequency(LPDWORD hz) {
  if (hz) *hz = 60;
  return DD_OK;
}

HRESULT DirectDraw::GetScanLine(LPDWORD line) {
  if (line) *line = 0;
  return DD_OK;
}

HRESULT DirectDraw::GetVerticalBlankStatus(LPBOOL in_vb) {
  if (in_vb) *in_vb = TRUE;
  return DD_OK;
}

HRESULT DirectDraw::Initialize(GUID*) { return DDERR_ALREADYINITIALIZED; }
HRESULT DirectDraw::RestoreDisplayMode() { return DD_OK; }

HRESULT DirectDraw::SetCooperativeLevel(HWND hwnd, DWORD flags) {
  DD_LOCK;
  Log("SetCooperativeLevel hwnd=%p flags=%lx", static_cast<void*>(hwnd), flags);
  if (hwnd) hwnd_ = hwnd;
  if ((flags & DDSCL_FULLSCREEN) && hwnd_) {
    if (!AttachWindow(hwnd_)) Log("could not attach to the game window");
  }
  return DD_OK;
}

HRESULT DirectDraw::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) {
  DD_LOCK;
  Log("SetDisplayMode %lux%lux%lu (virtual)", w, h, bpp);
  crash::OnModeChange(w, h);
  if (!w || !h) return DDERR_INVALIDMODE;
  mode_w_ = w;
  mode_h_ = h;
  mode_bpp_ = bpp ? bpp : 16;
  mode_format_ = FormatFor(mode_bpp_);
  if (hwnd_) AttachWindow(hwnd_);
  return DD_OK;
}

HRESULT DirectDraw::WaitForVerticalBlank(DWORD, HANDLE) {
  WaitVBlank();
  return DD_OK;
}

HRESULT DirectDraw::GetAvailableVidMem(LPDDSCAPS2, LPDWORD total, LPDWORD free_mem) {
  if (total) *total = 256u << 20;
  if (free_mem) *free_mem = 256u << 20;
  return DD_OK;
}

HRESULT DirectDraw::GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE7*) { return DDERR_NOTFOUND; }
HRESULT DirectDraw::RestoreAllSurfaces() { return DD_OK; }
HRESULT DirectDraw::TestCooperativeLevel() { return DD_OK; }

HRESULT DirectDraw::GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 id, DWORD) {
  if (!id) return DDERR_INVALIDPARAMS;
  InitDevice();
  std::memset(id, 0, sizeof(*id));
  Backend& b = B();
  std::snprintf(id->szDriver, sizeof(id->szDriver), "mcm2fix");
  std::snprintf(id->szDescription, sizeof(id->szDescription), "%s", b.adapter_name);
  id->liDriverVersion.QuadPart = 0x0006000E00010000LL;  // 6.14.1.0
  id->dwVendorId = b.vendor_id;
  id->dwDeviceId = b.device_id;
  id->guidDeviceIdentifier = {0x6d636d32, 0x6669, 0x7800, {0x44, 0x33, 0x44, 0x31, 0x31, 0, 0, 1}};
  return DD_OK;
}

HRESULT DirectDraw::StartModeTest(LPSIZE, DWORD, DWORD) { return DDERR_UNSUPPORTED; }
HRESULT DirectDraw::EvaluateMode(DWORD, DWORD*) { return DDERR_UNSUPPORTED; }

// ------------------------------------------------------------------- Direct3D

HRESULT Direct3D::QueryInterface(REFIID riid, void** out) { return dd_->QueryInterface(riid, out); }
ULONG Direct3D::AddRef() { return dd_->AddRef(); }
ULONG Direct3D::Release() { return dd_->Release(); }

HRESULT Direct3D::EnumDevices(d3d7::LPD3DENUMDEVICESCALLBACK7 cb, void* ctx) {
  if (!cb) return DDERR_INVALIDPARAMS;
  struct Entry {
    const char* desc;
    const char* name;
    GUID guid;
    bool hw;
  };
  static const Entry entries[] = {
      {"Microsoft Direct3D RGB Software Emulation", "RGB Emulation", d3d7::IID_IDirect3DRGBDevice, false},
      {"Microsoft Direct3D Hardware acceleration through Direct3D HAL", "Direct3D HAL",
       d3d7::IID_IDirect3DHALDevice, true},
      {"Microsoft Direct3D Hardware Transform and Lighting acceleration capable device",
       "Direct3D T&L HAL", d3d7::IID_IDirect3DTnLHalDevice, true},
  };
  for (const Entry& e : entries) {
    d3d7::D3DDEVICEDESC7 desc;
    Device::FillCaps(desc, e.guid);
    char d[128], n[64];
    std::snprintf(d, sizeof(d), "%s", e.desc);
    std::snprintf(n, sizeof(n), "%s", e.name);
    if (cb(d, n, &desc, ctx) == D3DENUMRET_CANCEL) break;
  }
  return DD_OK;
}

HRESULT Direct3D::CreateDevice(REFCLSID cls, IDirectDrawSurface7* rt, d3d7::IDirect3DDevice7** out) {
  DD_LOCK;
  if (!out) return DDERR_INVALIDPARAMS;
  *out = nullptr;
  Surface* target = Surface::From(rt);
  if (!target) return DDERR_INVALIDPARAMS;
  if (!InitDevice()) return DDERR_GENERIC;
  Device* dev = new Device(dd_, target, cls);
  if (!dev->Init()) {
    dev->Release();
    return DDERR_GENERIC;
  }
  Log("CreateDevice %ux%u", target->Width(), target->Height());
  *out = dev;
  return D3D_OK;
}

HRESULT Direct3D::CreateVertexBuffer(d3d7::D3DVERTEXBUFFERDESC* desc,
                                     d3d7::IDirect3DVertexBuffer7** out, DWORD) {
  if (!desc || !out) return DDERR_INVALIDPARAMS;
  *out = new VertexBuffer(*desc);
  return D3D_OK;
}

HRESULT Direct3D::EnumZBufferFormats(REFCLSID, d3d7::LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) {
  if (!cb) return DDERR_INVALIDPARAMS;
  const DWORD formats[][3] = {{16, 0xffff, 0}, {32, 0xffffff, 0}, {32, 0xffffff, 0xff000000}};
  for (const auto& f : formats) {
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_ZBUFFER | (f[2] ? DDPF_STENCILBUFFER : 0);
    pf.dwZBufferBitDepth = f[0];
    pf.dwStencilBitDepth = f[2] ? 8 : 0;
    pf.dwZBitMask = f[1];
    pf.dwStencilBitMask = f[2];
    if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
  }
  return D3D_OK;
}

HRESULT Direct3D::EvictManagedTextures() { return D3D_OK; }

// ---------------------------------------------------------------------- entry

HRESULT WINAPI CreateDirectDraw(GUID*, LPVOID* out, REFIID riid, IUnknown*) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = nullptr;
  if (!InitDevice()) {
    Log("no D3D11 device; DirectDrawCreateEx fails");
    return DDERR_NODIRECTDRAWHW;
  }
  DirectDraw* dd = new DirectDraw();
  HRESULT hr = dd->QueryInterface(riid, out);
  dd->Release();
  Log("DirectDrawCreateEx -> %08lx", hr);
  return hr;
}

}  // namespace dd
