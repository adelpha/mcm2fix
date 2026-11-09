// IDirectDrawSurface7 (plus palette, clipper and gamma control) for the
// D3D11 backend.
//
// Every surface keeps a CPU copy in the game's own pixel format, so Lock,
// GetDC and software blits behave exactly as before. Surfaces that are drawn
// with or into get a B8G8R8A8 GPU texture as well; `sync` records which copy
// is newer, and each side is brought up to date only when it is needed.
#pragma once
#include <windows.h>
#include <ddraw.h>

#include <cstdint>
#include <string>
#include <vector>

#include "backend.h"

namespace dd {

class DirectDraw;

// Channel layout of a pixel format, for conversion to and from 8 bits.
struct PixelLayout {
  UINT bpp = 16;
  DWORD mask[4] = {};  // r g b a
  int shift[4] = {};
  int bits[4] = {};
  bool depth = false;
  bool palette = false;  // 8-bit palette indices
  static PixelLayout From(const DDPIXELFORMAT& pf);
  // Unpack one pixel to 8-bit B, G, R, A.
  void ToBGRA(DWORD px, uint8_t out[4]) const;
  DWORD FromBGRA(const uint8_t in[4]) const;
};

DDPIXELFORMAT MakeRGBFormat(UINT bpp, DWORD r, DWORD g, DWORD b, DWORD a);

class Surface : public IDirectDrawSurface7 {
 public:
  Surface(DirectDraw* dd, const DDSURFACEDESC2& desc, Surface* root);
  virtual ~Surface();

  // Creates a surface and its implicit attachments (back buffer, mip levels).
  static HRESULT Create(DirectDraw* dd, const DDSURFACEDESC2& desc, Surface** out);

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  // IDirectDrawSurface7
  STDMETHOD(AddAttachedSurface)(LPDIRECTDRAWSURFACE7 s) override;
  STDMETHOD(AddOverlayDirtyRect)(LPRECT) override;
  STDMETHOD(Blt)(LPRECT dr, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD flags, LPDDBLTFX fx) override;
  STDMETHOD(BltBatch)(LPDDBLTBATCH, DWORD, DWORD) override;
  STDMETHOD(BltFast)(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD flags) override;
  STDMETHOD(DeleteAttachedSurface)(DWORD flags, LPDIRECTDRAWSURFACE7 s) override;
  STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb) override;
  STDMETHOD(EnumOverlayZOrders)(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) override;
  STDMETHOD(Flip)(LPDIRECTDRAWSURFACE7 target, DWORD flags) override;
  STDMETHOD(GetAttachedSurface)(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out) override;
  STDMETHOD(GetBltStatus)(DWORD) override;
  STDMETHOD(GetCaps)(LPDDSCAPS2 caps) override;
  STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER* out) override;
  STDMETHOD(GetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
  STDMETHOD(GetDC)(HDC* dc) override;
  STDMETHOD(GetFlipStatus)(DWORD) override;
  STDMETHOD(GetOverlayPosition)(LPLONG, LPLONG) override;
  STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE* out) override;
  STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) override;
  STDMETHOD(GetSurfaceDesc)(LPDDSURFACEDESC2 desc) override;
  STDMETHOD(Initialize)(LPDIRECTDRAW, LPDDSURFACEDESC2) override;
  STDMETHOD(IsLost)() override;
  STDMETHOD(Lock)(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD flags, HANDLE) override;
  STDMETHOD(ReleaseDC)(HDC dc) override;
  STDMETHOD(Restore)() override;
  STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER c) override;
  STDMETHOD(SetColorKey)(DWORD flags, LPDDCOLORKEY key) override;
  STDMETHOD(SetOverlayPosition)(LONG, LONG) override;
  STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE p) override;
  STDMETHOD(Unlock)(LPRECT) override;
  STDMETHOD(UpdateOverlay)(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX) override;
  STDMETHOD(UpdateOverlayDisplay)(DWORD) override;
  STDMETHOD(UpdateOverlayZOrder)(DWORD, LPDIRECTDRAWSURFACE7) override;
  STDMETHOD(GetDDInterface)(LPVOID* out) override;
  STDMETHOD(PageLock)(DWORD) override;
  STDMETHOD(PageUnlock)(DWORD) override;
  STDMETHOD(SetSurfaceDesc)(LPDDSURFACEDESC2, DWORD) override;
  STDMETHOD(SetPrivateData)(REFGUID, LPVOID, DWORD, DWORD) override;
  STDMETHOD(GetPrivateData)(REFGUID, LPVOID, LPDWORD) override;
  STDMETHOD(FreePrivateData)(REFGUID) override;
  STDMETHOD(GetUniquenessValue)(LPDWORD v) override;
  STDMETHOD(ChangeUniquenessValue)() override;
  STDMETHOD(SetPriority)(DWORD) override;
  STDMETHOD(GetPriority)(LPDWORD p) override;
  STDMETHOD(SetLOD)(DWORD) override;
  STDMETHOD(GetLOD)(LPDWORD l) override;

  // ---- backend helpers ----
  static Surface* From(IDirectDrawSurface7* s);  // nullptr if not ours
  // A temporary copy of a surface from the system DirectDraw (caller releases).
  static Surface* CopyForeign(DirectDraw* dd, IUnknown* s);
  UINT Width() const { return desc_.dwWidth; }
  UINT Height() const { return desc_.dwHeight; }
  DWORD Caps() const { return desc_.ddsCaps.dwCaps; }
  bool IsDepth() const { return layout_.depth; }
  bool IsRenderTargetKind() const;
  bool IsVisible() const { return (desc_.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER) != 0; }
  void SetUsedAsRenderTarget() { rt_used_ = true; }
  const PixelLayout& Layout() const { return layout_; }
  bool HasColorKey() const { return has_key_; }
  void KeyColor(float out[3]) const;
  Surface* ZBuffer() const { return zbuffer_; }

  // Make the GPU copy current (uploading CPU changes) and return it.
  GpuTexture* Gpu();
  // The same, for the device drawing 3D into it: leaves a multisampled
  // copy of a render target as it is (see msaa.h).
  GpuTexture* GpuForDraw();
  // The caller is about to draw into this surface on the GPU.
  void MarkGpuWritten();
  // Make the CPU copy current (reading back GPU changes).
  void SyncToCpu(const char* why = "other");
  void MarkCpuWritten();

  // The texture to sample when drawing with this surface: its GPU copy,
  // or a replacement picture (see texreplace.h).
  ID3D11ShaderResourceView* SampleSrv();

  // Copy pixel data (all mip levels when both have them) for Device::Load.
  void CopyFrom(Surface* src, const POINT* dst_pt, const RECT* src_rect);
  void InheritName(Surface* src);

 private:
  friend class DirectDraw;
  Surface* Root() { return root_ ? root_ : this; }
  // Mip levels share one GPU texture, owned by the top level.
  Surface* SyncOwner() { return level_ > 0 ? root_ : this; }
  bool InMipChain() const { return level_ > 0 || !mips_.empty(); }
  void PresentFront(UINT interval);
  void SwapContents(Surface* other);
  void AllocateMemory();
  void Upload();
  void Download();
  bool UseGpuFor(Surface* src);
  void BltGpu(const RECT& dr, Surface* src, const RECT& sr, bool key, DWORD key_value);
  void BltCpu(const RECT& dr, Surface* src, const RECT& sr, bool key, DWORD key_value);
  void FillGpu(const RECT& dr, DWORD color);
  void FillCpu(const RECT& dr, DWORD color);
  uint8_t* Pixel(LONG x, LONG y) { return mem_.data() + y * Pitch() + x * (layout_.bpp / 8); }
  LONG Pitch() const { return desc_.lPitch; }
  void PaletteBGRA(uint8_t out[256][4]) const;

  LONG refs_ = 1;
  DirectDraw* dd_;
  Surface* root_;  // owner of an implicit attachment (back buffer, mip level)
  DDSURFACEDESC2 desc_{};
  PixelLayout layout_;
  std::vector<uint8_t> mem_;

  Surface* back_ = nullptr;        // flip chain
  std::vector<Surface*> mips_;     // levels 1.. (root only)
  int level_ = 0;
  Surface* zbuffer_ = nullptr;     // explicitly attached, AddRef'd
  bool has_key_ = false;
  DDCOLORKEY key_{};
  IDirectDrawPalette* palette_ = nullptr;
  IDirectDrawClipper* clipper_ = nullptr;

  GpuTexture gpu_;
  GpuTexture replaced_;    // replacement picture, if one matches
  uint64_t tex_hash_ = 0;  // hash of the last uploaded top level
  std::string tex_name_;   // the game's name for this texture (TextureKey), if known
  void MatchReplacement(const uint8_t* bgra, UINT w, UINT h);
  enum Sync { kInSync, kCpuNewer, kGpuNewer } sync_ = kCpuNewer;

  // GetDC state.
  HDC dc_ = nullptr;
  HBITMAP dib_ = nullptr;
  HGDIOBJ old_bitmap_ = nullptr;
  void* dib_bits_ = nullptr;
  unsigned uploads_ = 0;  // texture uploads so far (edge fix-up is skipped for streaming textures)
  IDXGISurface1* gdi_surface_ = nullptr;  // set while GDI draws on the GPU copy
  bool GetGpuDC(HDC* out);
  void ReleaseGpuDC();
  bool locked_ = false;
  bool lock_readonly_ = false;
  bool rt_used_ = false;
  DWORD priority_ = 0, lod_ = 0;
  DWORD uniqueness_ = 1;
};

class Palette final : public IDirectDrawPalette {
 public:
  Palette(DWORD caps, const PALETTEENTRY* entries);
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetCaps)(LPDWORD caps) override;
  STDMETHOD(GetEntries)(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY out) override;
  STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override;
  STDMETHOD(SetEntries)(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY in) override;
  PALETTEENTRY entries[256] = {};

 private:
  LONG refs_ = 1;
  DWORD caps_;
};

class Clipper final : public IDirectDrawClipper {
 public:
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetClipList)(LPRECT, LPRGNDATA, LPDWORD) override;
  STDMETHOD(GetHWnd)(HWND* out) override;
  STDMETHOD(Initialize)(LPDIRECTDRAW, DWORD) override;
  STDMETHOD(IsClipListChanged)(BOOL* out) override;
  STDMETHOD(SetClipList)(LPRGNDATA, DWORD) override;
  STDMETHOD(SetHWnd)(DWORD, HWND h) override;

 private:
  LONG refs_ = 1;
  HWND hwnd_ = nullptr;
};

class GammaControl final : public IDirectDrawGammaControl {
 public:
  explicit GammaControl(Surface* owner) : owner_(owner) {}
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;
  STDMETHOD(GetGammaRamp)(DWORD, LPDDGAMMARAMP ramp) override;
  STDMETHOD(SetGammaRamp)(DWORD, LPDDGAMMARAMP ramp) override;

 private:
  LONG refs_ = 1;
  Surface* owner_;
  DDGAMMARAMP ramp_{};
  bool set_ = false;
};

}  // namespace dd
