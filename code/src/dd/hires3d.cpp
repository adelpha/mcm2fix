// MenuHiRes3D: see hires3d.h.
#include "hires3d.h"

#include <windows.h>

#include <algorithm>

#include "../config.h"
#include "gputext.h"
#include "msaa.h"

namespace dd {

namespace {

struct State {
  UINT samples = 0;  // 0: not chosen yet
  ID3D11Texture2D* color = nullptr;  // multisampled (or plain when samples == 1)
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  GpuTexture resolved;  // single-sample copy that is put into the overlay
  UINT width = 0, height = 0;      // window-sized buffers
  UINT base_w = 0, base_h = 0;     // the surface they mirror
  float sx = 1, sy = 1;
  const void* owner = nullptr;     // texture being mirrored (not AddRef'd)
  RECT dirty{};                    // surface pixels drawn since the last put
  bool has_dirty = false;
  bool race = false;
  bool failed = false;
  // Statistics, logged every few seconds while in use.
  unsigned draws = 0, clears = 0, puts = 0, switches = 0, refusals = 0;
  DWORD last_report = 0;
  bool reported_first = false;
};
State g;

void ReleaseBuffers() {
  SafeRelease(g.rtv);
  SafeRelease(g.color);
  SafeRelease(g.dsv);
  SafeRelease(g.depth);
  g.resolved.Reset();
  g.width = g.height = 0;
}

void Report(bool force) {
  const DWORD now = GetTickCount();
  if (!force && now - g.last_report < 3000) return;
  if (g.draws || g.clears || g.puts || g.refusals || force) {
    Log("hi-res menu 3D: %u draws, %u clears, %u puts into the overlay, %u target switches, %u refused, buffers "
        "%ux%u x%u for %ux%u (x%.2f)",
        g.draws, g.clears, g.puts, g.switches, g.refusals, g.width, g.height, g.samples, g.base_w, g.base_h, g.sx);
  }
  g.draws = g.clears = g.puts = g.switches = g.refusals = 0;
  g.last_report = now;
}

UINT Samples() {
  if (!g.samples) {
    // Same antialiasing as the 640x480 drawing, at most 4x (the buffers are
    // window-sized).
    UINT n = std::min<UINT>(MsaaSamples(), 4);
    UINT q1 = 0, q2 = 0;
    while (n > 1 && !(SUCCEEDED(B().device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &q1)) &&
                      SUCCEEDED(B().device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q2)) &&
                      q1 > 0 && q2 > 0))
      n /= 2;
    g.samples = std::max<UINT>(n, 1);
  }
  return g.samples;
}

bool EnsureBuffers(UINT w, UINT h) {
  if (g.color && g.width == w && g.height == h) return true;
  ReleaseBuffers();
  ID3D11Device* dev = B().device;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = Samples();
  td.Usage = D3D11_USAGE_DEFAULT;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | (g.samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
  bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g.color)) &&
            SUCCEEDED(dev->CreateRenderTargetView(g.color, nullptr, &g.rtv));
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &g.depth)) &&
       SUCCEEDED(dev->CreateDepthStencilView(g.depth, nullptr, &g.dsv));
  ok = ok && CreateGpuTexture(g.resolved, w, h, 1, true, false, false);
  if (!ok) {
    Log("hi-res menu 3D: %ux%u buffers (x%u MSAA) could not be made; menu 3D stays 640x480", w, h, g.samples);
    ReleaseBuffers();
    g.failed = true;
    return false;
  }
  const float black[4] = {0, 0, 0, 1};
  B().ctx->ClearRenderTargetView(g.rtv, black);
  B().ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  g.width = w;
  g.height = h;
  Log("hi-res menu 3D: buffers %ux%u, %ux MSAA (%.1f MB)", w, h, g.samples,
      (static_cast<double>(w) * h * (8.0 * g.samples + 4.0)) / (1024.0 * 1024.0));
  return true;
}

void Put() {
  if (!g.owner || !g.has_dirty || !g.color) return;
  ID3D11DeviceContext* ctx = B().ctx;
  if (g.samples > 1)
    ctx->ResolveSubresource(g.resolved.tex, 0, g.color, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
  else
    ctx->CopyResource(g.resolved.tex, g.color);
  const bool ok = GpuTextPut(g.owner, g.base_w, g.base_h, g.resolved, g.dirty);
  ContextTouched();
  if (ok) {
    ++g.puts;
  } else {
    LogOnce("hi-res menu 3D: could not put the drawing into the overlay (%ld,%ld-%ld,%ld)", g.dirty.left, g.dirty.top,
            g.dirty.right, g.dirty.bottom);
  }
  g.has_dirty = false;
}

}  // namespace

bool Hi3dPrepare(const GpuTexture& rt, UINT w, UINT h) {
  const Config& c = GetConfig();
  if (!c.menu_hires_3d || g.failed || IsD3D9() || !rt.Valid()) return false;
  auto refuse = [&](const char* why) {
    ++g.refusals;
    LogOnce("hi-res menu 3D: 3D drawn into a %ux%u target is left as it is: %s", w, h, why);
    Report(false);
    return false;
  };
  if (g.race) {
    if (g.color) {
      Hi3dRelease(g.owner);
      ReleaseBuffers();
      g.owner = nullptr;
      Log("hi-res menu 3D: race running, buffers freed");
    }
    return refuse("a race is running");
  }
  if (w > 640 || h > 480) return refuse("bigger than the 640x480 menus");
  UINT ow = 0, oh = 0;
  if (!GpuTextOverlaySize(w, h, &ow, &oh)) return refuse("GpuText is off");
  if (ow < w + w / 16) return refuse("the window is not bigger than 640x480");
  const void* n = rt.Native();
  if (n != g.owner) {
    Hi3dRelease(g.owner);
    g.owner = n;
    ++g.switches;
  }
  if (!EnsureBuffers(ow, oh)) {
    g.owner = nullptr;
    return false;
  }
  g.base_w = w;
  g.base_h = h;
  g.sx = static_cast<float>(ow) / w;
  g.sy = static_cast<float>(oh) / h;
  if (!g.reported_first) {
    g.reported_first = true;
    Log("hi-res menu 3D: first 3D in the menus, drawn again at %ux%u (x%.2f)", ow, oh, g.sx);
  }
  Report(false);
  return true;
}

bool Hi3dViews(ID3D11RenderTargetView** rtv, ID3D11DepthStencilView** dsv, float* sx, float* sy, UINT* w, UINT* h) {
  if (!g.owner || !g.rtv) return false;
  *rtv = g.rtv;
  *dsv = g.dsv;
  *sx = g.sx;
  *sy = g.sy;
  *w = g.width;
  *h = g.height;
  return true;
}

void Hi3dDrawn(const RECT& r, bool clear) {
  if (!g.owner) return;
  ++(clear ? g.clears : g.draws);
  RECT c{std::max<LONG>(r.left, 0), std::max<LONG>(r.top, 0), std::min<LONG>(r.right, static_cast<LONG>(g.base_w)),
         std::min<LONG>(r.bottom, static_cast<LONG>(g.base_h))};
  if (c.left >= c.right || c.top >= c.bottom) return;
  if (!g.has_dirty) {
    g.dirty = c;
    g.has_dirty = true;
  } else {
    UnionRect(&g.dirty, &g.dirty, &c);
  }
}

void Hi3dRelease(const void* native) {
  if (!native || native != g.owner) return;
  Put();
}

void Hi3dForget(const void* native) {
  if (native && native == g.owner) {
    g.owner = nullptr;
    g.has_dirty = false;
  }
}

void Hi3dRaceLive(bool live) {
  if (g.race != live) Log("hi-res menu 3D: race %s", live ? "started, off until it ends" : "ended");
  g.race = live;
}

}  // namespace dd
