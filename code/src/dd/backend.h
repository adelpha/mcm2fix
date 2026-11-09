// GPU core for the DirectDraw / Direct3D 7 stand-in: device, swap chain,
// borderless window, the 2D blit path and texture plumbing.
//
// Two APIs sit behind it (ini Renderer): Direct3D 11 (default; shaders, see
// backend.cpp) and Direct3D 9 (fixed-function pipeline, no shaders, works on
// Windows XP; see backend9.cpp). The rest of src/dd/ asks ActiveApi() only
// where the two really differ.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

// Direct3D 9 types, declared only: d3d9.h's macros clash with the Direct3D 7
// names in d3d7.h, so only the D3D9 files include it (see backend9.h).
struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DBaseTexture9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;

#include <cstdint>
#include <mutex>

namespace dd {

// One global lock: the game calls DirectDraw from more than one thread, and
// the D3D11 immediate context is single-threaded.
std::recursive_mutex& Lock();
#define DD_LOCK std::lock_guard<std::recursive_mutex> dd_lock_(::dd::Lock())

void Log(const char* fmt, ...);
// Log a message only the first time this exact text is seen.
void LogOnce(const char* fmt, ...);

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

enum class Api { kD3D11, kD3D9 };
// Which API the renderer runs on (decided by InitDevice).
Api ActiveApi();
inline bool IsD3D9() { return ActiveApi() == Api::kD3D9; }

// A GPU copy of a surface. All colour data is B8G8R8A8 on the GPU, whatever
// the game's 16-bit format; conversion happens on upload and readback.
struct GpuTexture {
  // Direct3D 11
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  // Direct3D 9: a texture (colour), and its top level when it is a render
  // target, or a depth-stencil surface (depth).
  IDirect3DTexture9* tex9 = nullptr;
  IDirect3DSurface9* surf9 = nullptr;
  DWORD fmt9 = 0;  // D3DFORMAT
  bool pool_default = false;  // D3D9: lost with the device (render targets, depth)
  UINT width = 0, height = 0, levels = 0;
  bool depth = false;
  bool rt = false;   // can be drawn into
  bool gdi = false;  // GDI can draw on it directly (GpuGetDC)
  void Reset();
  // The same resources, AddRef'd (for sharing one GPU copy).
  GpuTexture Share() const;
  // Identity of the underlying resource (MSAA bookkeeping).
  const void* Native() const { return tex ? static_cast<const void*>(tex) : static_cast<const void*>(tex9 ? static_cast<const void*>(tex9) : surf9); }
  bool Valid() const { return tex || tex9 || surf9; }
  bool CanRender() const { return rtv || (rt && surf9); }
  bool CanSample() const { return srv || tex9; }
};

struct Backend {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain1* swapchain = nullptr;
  ID3D11RenderTargetView* swap_rtv = nullptr;
  UINT swap_w = 0, swap_h = 0;
  HWND hwnd = nullptr;
  char adapter_name[128] = "Direct3D 11";
  UINT vendor_id = 0, device_id = 0;

  // 2D blit pipeline.
  ID3D11VertexShader* blit_vs = nullptr;
  ID3D11PixelShader* blit_ps = nullptr;
  ID3D11Buffer* blit_cb = nullptr;
  ID3D11SamplerState* point_sampler = nullptr;
  ID3D11SamplerState* linear_sampler = nullptr;
  ID3D11RasterizerState* blit_raster = nullptr;
  ID3D11DepthStencilState* no_depth = nullptr;
  ID3D11BlendState* no_blend = nullptr;
  ID3D11BlendState* alpha_only = nullptr;

  // Direct3D 9
  IDirect3D9* d3d9 = nullptr;
  IDirect3DDevice9* dev9 = nullptr;
  UINT adapter9 = 0;
};

Backend& B();
bool Ready();

// Creates the device on the high-performance GPU. Safe to call repeatedly.
bool InitDevice();
// Direct3D 9 versions (backend9.cpp), called from the functions here.
bool InitDevice9();
// Attaches the swap chain to the game window and makes it borderless,
// covering the monitor it is on.
bool AttachWindow(HWND hwnd);
// Re-apply the borderless window if the game moved or resized it.
void KeepBorderless();

// Compile HLSL with the system d3dcompiler_47.dll.
ID3DBlob* Compile(const char* source, const char* entry, const char* target);

// `alpha`: D3D9 picks a format without alpha for render targets that need
// none, so GDI can draw on them.
bool CreateGpuTexture(GpuTexture& t, UINT w, UINT h, UINT levels, bool render_target,
                      bool depth, bool alpha = true);
// A sampled-only texture with a full mip chain made from `bgra` (w*h*4).
bool CreateTextureFromBGRA(GpuTexture& t, const uint8_t* bgra, UINT w, UINT h);
// Replace mip level `level` (w x h) with tightly packed B8G8R8A8 pixels.
void UploadLevel(GpuTexture& t, UINT level, const uint8_t* bgra, UINT w, UINT h);
// Read the top level back as B8G8R8A8; calls row(y, pixels) per row.
bool DownloadTop(GpuTexture& t, void (*row)(void* ctx, UINT y, const uint8_t* px), void* ctx);
// dst = a copy of src's top level (dst created to match).
bool CopyTexture(GpuTexture& dst, const GpuTexture& src);
// Set every depth value of a depth texture.
void ClearDepth(GpuTexture& t, float z);
// GDI straight onto a render-target texture (no readback); false if the
// API or format does not allow it.
bool GpuGetDC(GpuTexture& t, HDC* dc, void** handle);
void GpuReleaseDC(GpuTexture& t, HDC dc, void* handle);

// Draw src (or a solid colour when src is null) into dst_rtv.
// Rects are in pixels of the respective textures.
struct BlitParams {
  ID3D11RenderTargetView* dst = nullptr;
  IDirect3DSurface9* dst9 = nullptr;
  UINT dst_w = 0, dst_h = 0;
  RECT dst_rect{};
  ID3D11ShaderResourceView* src = nullptr;
  IDirect3DBaseTexture9* src9 = nullptr;
  UINT src_w = 1, src_h = 1;
  RECT src_rect{};
  bool color_key = false;  // discard texels equal to key
  float key[3] = {0, 0, 0};
  bool linear = false;
  float fill[4] = {0, 0, 0, 1};
  bool opaque = true;  // write alpha 1 (destination format has no alpha)
  bool alpha_only = false;  // write only the alpha channel
  // D3D9 colour key: src9 is a keyed copy whose key texels have alpha 0.
  bool alpha_key = false;
  bool HasSrc() const { return src || src9; }
  void SetDst(const GpuTexture& t) {
    dst = t.rtv;
    dst9 = t.surf9;
    dst_w = t.width;
    dst_h = t.height;
  }
  void SetSrc(const GpuTexture& t);
};
void Blit(const BlitParams& p);
void Blit9(const BlitParams& p);

// Scale src (game resolution) into the window, keeping its aspect ratio,
// and present.
void Present(const GpuTexture& src, UINT src_w, UINT src_h, UINT interval);
void Present9(const GpuTexture& src, UINT src_w, UINT src_h, UINT interval);
// Per-second activity counters, written to the log (helps debugging).
struct Stats {
  unsigned flips, presents, draws, blts, foreign, front_writes, locks, downloads;
  unsigned foreign_sum;  // checksum of the last copied video frame
};
Stats& Counters();
// Record why a surface had to be read back from the GPU.
void NoteReadback(const char* why, DWORD caps, UINT w, UINT h);

// Block until the next vertical blank of the game's monitor.
void WaitVBlank();
void WaitVBlank9();
bool AttachWindow9(HWND hwnd);
// True while the D3D9 device is lost (nothing can be drawn).
bool DeviceLost9();
// Shared pieces of the window handling (backend.cpp).
void ApplyBorderless(HWND hwnd);
void ReportStats();

}  // namespace dd
