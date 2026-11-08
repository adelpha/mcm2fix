// IDirectDraw7 and IDirect3D7 for the D3D11 backend.
//
// Display modes are virtual: SetDisplayMode only records the game's
// resolution. Its frames are scaled into a borderless window covering the
// monitor, so the desktop resolution never changes.
#pragma once
#include <windows.h>
#include <ddraw.h>

#include "d3d7.h"

namespace dd {

class Surface;
class DirectDraw;

class Direct3D : public d3d7::IDirect3D7 {
 public:
  explicit Direct3D(DirectDraw* dd) : dd_(dd) {}
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(EnumDevices)(d3d7::LPD3DENUMDEVICESCALLBACK7 cb, void* ctx) override;
  STDMETHOD(CreateDevice)(REFCLSID cls, IDirectDrawSurface7* rt,
                          d3d7::IDirect3DDevice7** out) override;
  STDMETHOD(CreateVertexBuffer)(d3d7::D3DVERTEXBUFFERDESC* desc,
                                d3d7::IDirect3DVertexBuffer7** out, DWORD flags) override;
  STDMETHOD(EnumZBufferFormats)(REFCLSID dev, d3d7::LPD3DENUMPIXELFORMATSCALLBACK cb,
                                void* ctx) override;
  STDMETHOD(EvictManagedTextures)() override;

 private:
  DirectDraw* dd_;
};

class DirectDraw : public IDirectDraw7 {
 public:
  DirectDraw();
  virtual ~DirectDraw();

  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(Compact)() override;
  STDMETHOD(CreateClipper)(DWORD flags, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) override;
  STDMETHOD(CreatePalette)(DWORD flags, LPPALETTEENTRY entries, LPDIRECTDRAWPALETTE* out,
                           IUnknown* outer) override;
  STDMETHOD(CreateSurface)(LPDDSURFACEDESC2 desc, LPDIRECTDRAWSURFACE7* out,
                           IUnknown* outer) override;
  STDMETHOD(DuplicateSurface)(LPDIRECTDRAWSURFACE7 src, LPDIRECTDRAWSURFACE7* out) override;
  STDMETHOD(EnumDisplayModes)(DWORD flags, LPDDSURFACEDESC2 desc, LPVOID ctx,
                              LPDDENUMMODESCALLBACK2 cb) override;
  STDMETHOD(EnumSurfaces)(DWORD flags, LPDDSURFACEDESC2 desc, LPVOID ctx,
                          LPDDENUMSURFACESCALLBACK7 cb) override;
  STDMETHOD(FlipToGDISurface)() override;
  STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) override;
  STDMETHOD(GetDisplayMode)(LPDDSURFACEDESC2 desc) override;
  STDMETHOD(GetFourCCCodes)(LPDWORD n, LPDWORD codes) override;
  STDMETHOD(GetGDISurface)(LPDIRECTDRAWSURFACE7* out) override;
  STDMETHOD(GetMonitorFrequency)(LPDWORD hz) override;
  STDMETHOD(GetScanLine)(LPDWORD line) override;
  STDMETHOD(GetVerticalBlankStatus)(LPBOOL in_vb) override;
  STDMETHOD(Initialize)(GUID* guid) override;
  STDMETHOD(RestoreDisplayMode)() override;
  STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override;
  STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD flags) override;
  STDMETHOD(WaitForVerticalBlank)(DWORD flags, HANDLE ev) override;
  STDMETHOD(GetAvailableVidMem)(LPDDSCAPS2 caps, LPDWORD total, LPDWORD free_mem) override;
  STDMETHOD(GetSurfaceFromDC)(HDC dc, LPDIRECTDRAWSURFACE7* out) override;
  STDMETHOD(RestoreAllSurfaces)() override;
  STDMETHOD(TestCooperativeLevel)() override;
  STDMETHOD(GetDeviceIdentifier)(LPDDDEVICEIDENTIFIER2 id, DWORD flags) override;
  STDMETHOD(StartModeTest)(LPSIZE modes, DWORD n, DWORD flags) override;
  STDMETHOD(EvaluateMode)(DWORD flags, DWORD* timeout) override;

  // ---- backend helpers ----
  const DDPIXELFORMAT& ModeFormat() const { return mode_format_; }
  UINT ModeWidth() const { return mode_w_; }
  UINT ModeHeight() const { return mode_h_; }
  Direct3D* D3D() { return &d3d_; }
  // The DirectDraw object currently alive (surfaces may outlive it).
  static DirectDraw* Current();
  // The primary surface registers itself so GetGDISurface can find it.
  void SetPrimary(Surface* s) { primary_ = s; }
  void ForgetSurface(Surface* s) {
    if (primary_ == s) primary_ = nullptr;
  }

 private:
  LONG refs_ = 1;
  Direct3D d3d_;
  UINT mode_w_ = 640, mode_h_ = 480, mode_bpp_ = 16;
  DDPIXELFORMAT mode_format_{};
  Surface* primary_ = nullptr;
  HWND hwnd_ = nullptr;
};

// The hook for Mcm2.exe's DirectDrawCreateEx import.
HRESULT WINAPI CreateDirectDraw(GUID* guid, LPVOID* out, REFIID riid, IUnknown* outer);

}  // namespace dd
