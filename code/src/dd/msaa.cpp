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
  ID3D11Texture2D* owner = nullptr;  // the texture being mirrored (not AddRef'd)
  bool dirty = false;                // MSAA copy newer than the owner
  bool pending_load = false;         // owner's contents not copied in yet
};
State g;

void ReleaseBuffers() {
  SafeRelease(g.rtv);
  SafeRelease(g.color);
  SafeRelease(g.dsv);
  SafeRelease(g.depth);
  g.width = g.height = 0;
}

bool Supported(UINT n) {
  UINT q1 = 0, q2 = 0;
  ID3D11Device* dev = B().device;
  return SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &q1)) &&
         SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q2)) &&
         q1 > 0 && q2 > 0;
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
  if (!rt || !rt->tex || !rt->srv || MsaaSamples() <= 1) return false;
  if (!EnsureBuffers(rt->width, rt->height)) return false;
  if (g.owner != rt->tex) {
    MsaaRelease(g.owner);
    g.owner = rt->tex;
    g.dirty = false;
    g.pending_load = true;
  }
  if (g.pending_load) {
    if (!full_clear) {
      // Bring in what is already in the target (a backdrop, an earlier pass).
      BlitParams p;
      p.dst = g.rtv;
      p.dst_w = g.width;
      p.dst_h = g.height;
      p.dst_rect = {0, 0, static_cast<LONG>(g.width), static_cast<LONG>(g.height)};
      p.src = rt->srv;
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
  return true;
}

void MsaaMarkDrawn() {
  if (g.owner) g.dirty = true;
}

void MsaaRelease(ID3D11Texture2D* tex) {
  if (!tex || tex != g.owner) return;
  if (g.dirty && g.color)
    B().ctx->ResolveSubresource(g.owner, 0, g.color, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  g.owner = nullptr;
  g.dirty = false;
  g.pending_load = false;
}

void MsaaForget(ID3D11Texture2D* tex) {
  if (tex && tex == g.owner) {
    g.owner = nullptr;
    g.dirty = false;
    g.pending_load = false;
  }
}

}  // namespace dd
