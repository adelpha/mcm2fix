// D3D11 core for the DirectDraw / Direct3D 7 stand-in: device, swap chain,
// borderless window, shaders, state-object caches and the 2D blit path.
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

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

// A GPU copy of a surface. All colour data is B8G8R8A8 on the GPU, whatever
// the game's 16-bit format; conversion happens on upload and readback.
struct GpuTexture {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  UINT width = 0, height = 0, levels = 0;
  bool depth = false;
  void Reset();
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
};

Backend& B();
bool Ready();

// Creates the device on the high-performance GPU. Safe to call repeatedly.
bool InitDevice();
// Attaches the swap chain to the game window and makes it borderless,
// covering the monitor it is on.
bool AttachWindow(HWND hwnd);
// Re-apply the borderless window if the game moved or resized it.
void KeepBorderless();

// Compile HLSL with the system d3dcompiler_47.dll.
ID3DBlob* Compile(const char* source, const char* entry, const char* target);

bool CreateGpuTexture(GpuTexture& t, UINT w, UINT h, UINT levels, bool render_target,
                      bool depth);

// Draw src (or a solid colour when src is null) into dst_rtv.
// Rects are in pixels of the respective textures.
struct BlitParams {
  ID3D11RenderTargetView* dst = nullptr;
  UINT dst_w = 0, dst_h = 0;
  RECT dst_rect{};
  ID3D11ShaderResourceView* src = nullptr;
  UINT src_w = 1, src_h = 1;
  RECT src_rect{};
  bool color_key = false;  // discard texels equal to key
  float key[3] = {0, 0, 0};
  bool linear = false;
  float fill[4] = {0, 0, 0, 1};
  bool opaque = true;  // write alpha 1 (destination format has no alpha)
};
void Blit(const BlitParams& p);

// Scale src (game resolution) into the window, keeping its aspect ratio,
// and present.
void Present(ID3D11ShaderResourceView* src, UINT src_w, UINT src_h, UINT interval);
// Block until the next vertical blank of the game's monitor.
void WaitVBlank();

}  // namespace dd
