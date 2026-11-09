#include "backend9.h"
#include "msaa.h"

#include "../config.h"

namespace dd {

namespace {

struct State {
  bool checked = false;
  UINT samples = 1;
  ID3D11Texture2D* color = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  UINT width = 0, height = 0;
  // Direct3D 9
  IDirect3DSurface9* rt9 = nullptr;
  IDirect3DSurface9* ds9 = nullptr;
  DWORD fmt9 = 0;
  IDirect3DSurface9* owner_surf9 = nullptr;  // the mirrored texture's top level (not AddRef'd)
  const void* owner = nullptr;  // the texture being mirrored (not AddRef'd)
  bool dirty = false;                // MSAA copy newer than the owner
  bool pending_load = false;         // owner's contents not copied in yet
};
State g;

void ReleaseBuffers() {
  SafeRelease(g.rtv);
  SafeRelease(g.color);
  SafeRelease(g.dsv);
  SafeRelease(g.depth);
  SafeRelease(g.rt9);
  SafeRelease(g.ds9);
  g.width = g.height = 0;
}

bool Supported(UINT n) {
  if (IsD3D9()) {
    Backend& b = B();
    const auto type = static_cast<D3DMULTISAMPLE_TYPE>(n);
    DWORD q1 = 0, q2 = 0;
    HRESULT h1 = b.d3d9->CheckDeviceMultiSampleType(b.adapter9, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, type, &q1);
    HRESULT h2 = b.d3d9->CheckDeviceMultiSampleType(b.adapter9, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, type, &q2);
    Log("MSAA %ux: colour %s, depth %s", n, SUCCEEDED(h1) ? "ok" : "no", SUCCEEDED(h2) ? "ok" : "no");
    return SUCCEEDED(h1) && SUCCEEDED(h2);
  }
  UINT q1 = 0, q2 = 0;
  ID3D11Device* dev = B().device;
  return SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &q1)) &&
         SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q2)) &&
         q1 > 0 && q2 > 0;
}

bool EnsureBuffers9(UINT w, UINT h, DWORD fmt) {
  if (g.rt9 && g.width == w && g.height == h && g.fmt9 == fmt) return true;
  MsaaRelease(g.owner);
  ReleaseBuffers();
  IDirect3DDevice9* dev = B().dev9;
  const auto type = static_cast<D3DMULTISAMPLE_TYPE>(g.samples);
  HRESULT hr = dev->CreateRenderTarget(w, h, static_cast<D3DFORMAT>(fmt), type, 0, FALSE, &g.rt9, nullptr);
  if (FAILED(hr)) {
    Log("MSAA %ux colour buffer %ux%u failed (%08lx); antialiasing off", g.samples, w, h, hr);
    g.samples = 1;
    return false;
  }
  hr = dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, type, 0, FALSE, &g.ds9, nullptr);
  if (FAILED(hr)) {
    Log("MSAA %ux depth buffer %ux%u failed (%08lx); antialiasing off", g.samples, w, h, hr);
    ReleaseBuffers();
    g.samples = 1;
    return false;
  }
  g.width = w;
  g.height = h;
  g.fmt9 = fmt;
  LogOnce("MSAA %ux buffers %ux%u", g.samples, w, h);
  return true;
}

bool EnsureBuffers(UINT w, UINT h) {
  if (g.color && g.width == w && g.height == h) return true;
  MsaaRelease(g.owner);
  ReleaseBuffers();
  ID3D11Device* dev = B().device;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = g.samples;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.color))) {
    Log("MSAA %ux colour buffer %ux%u failed; antialiasing off", g.samples, w, h);
    g.samples = 1;
    return false;
  }
  dev->CreateRenderTargetView(g.color, nullptr, &g.rtv);
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.depth))) {
    Log("MSAA %ux depth buffer %ux%u failed; antialiasing off", g.samples, w, h);
    ReleaseBuffers();
    g.samples = 1;
    return false;
  }
  dev->CreateDepthStencilView(g.depth, nullptr, &g.dsv);
  if (!g.rtv || !g.dsv) {
    ReleaseBuffers();
    g.samples = 1;
    return false;
  }
  const float black[4] = {0, 0, 0, 1};
  B().ctx->ClearRenderTargetView(g.rtv, black);
  B().ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  g.width = w;
  g.height = h;
  LogOnce("MSAA %ux buffers %ux%u", g.samples, w, h);
  return true;
}

}  // namespace

UINT MsaaSamples() {
  if (!g.checked && Ready()) {
    g.checked = true;
    UINT want = static_cast<UINT>(GetConfig().msaa);
    UINT n = want >= 8 ? 8 : want >= 4 ? 4 : want >= 2 ? 2 : 1;
    while (n > 1 && !Supported(n)) n /= 2;
    g.samples = n;
    Log("antialiasing: %ux MSAA (asked for %u)", n, want);
  }
  return g.samples;
}

bool MsaaAcquire(GpuTexture* rt, MsaaViews& out, bool full_clear) {
  if (!rt || !rt->CanSample() || !rt->CanRender() || MsaaSamples() <= 1) return false;
  if (IsD3D9()) {
    if (!EnsureBuffers9(rt->width, rt->height, rt->fmt9)) return false;
  } else if (!EnsureBuffers(rt->width, rt->height)) {
    return false;
  }
  if (g.owner != rt->Native()) {
    MsaaRelease(g.owner);
    g.owner = rt->Native();
    g.owner_surf9 = rt->surf9;
    g.dirty = false;
    g.pending_load = true;
  }
  if (g.pending_load) {
    if (!full_clear) {
      // Bring in what is already in the target (a backdrop, an earlier pass).
      BlitParams p;
      p.dst = g.rtv;
      p.dst9 = g.rt9;
      p.dst_w = g.width;
      p.dst_h = g.height;
      p.dst_rect = {0, 0, static_cast<LONG>(g.width), static_cast<LONG>(g.height)};
      p.src = rt->srv;
      p.src9 = rt->tex9;
      p.src_w = rt->width;
      p.src_h = rt->height;
      p.src_rect = p.dst_rect;
      p.opaque = false;
      Blit(p);
    }
    g.pending_load = false;
  }
  out.rtv = g.rtv;
  out.dsv = g.dsv;
  out.rt9 = g.rt9;
  out.ds9 = g.ds9;
  return true;
}

void MsaaMarkDrawn() {
  if (g.owner) g.dirty = true;
}

void MsaaRelease(const void* tex) {
  if (!tex || tex != g.owner) return;
  if (g.dirty && g.color)
    B().ctx->ResolveSubresource(static_cast<ID3D11Texture2D*>(const_cast<void*>(g.owner)), 0,
                                g.color, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  if (g.dirty && g.rt9 && g.owner_surf9) {
    EndScene9();
    HRESULT hr = B().dev9->StretchRect(g.rt9, nullptr, g.owner_surf9, nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) LogOnce("MSAA resolve (StretchRect) failed %08lx", hr);
  }
  g.owner = nullptr;
  g.owner_surf9 = nullptr;
  g.dirty = false;
  g.pending_load = false;
}

void MsaaForget(const void* tex) {
  if (tex && tex == g.owner) {
    g.owner = nullptr;
    g.owner_surf9 = nullptr;
    g.dirty = false;
    g.pending_load = false;
  }
}

void MsaaDeviceLost() {
  g.owner = nullptr;
  g.owner_surf9 = nullptr;
  g.dirty = false;
  g.pending_load = false;
  ReleaseBuffers();
}

}  // namespace dd
