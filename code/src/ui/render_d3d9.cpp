// MenuMaX: see render_d3d9.h.
#include "render_d3d9.h"

#include <RmlUi/Core/Vertex.h>

#include <algorithm>
#include <cstring>

#include "../dd/backend9.h"

namespace ui {

namespace {

// Pre-transformed vertex (screen pixels), as the blitter uses.
struct V9 {
  float x, y, z, rhw;
  DWORD c;
  float u, v;
};
constexpr DWORD kFvf = dd::kFvf9XYZRHW | dd::kFvf9Diffuse | dd::kFvf9Tex1;

struct Geometry {
  std::vector<Rml::Vertex> verts;
  std::vector<uint16_t> idx16;
  std::vector<uint32_t> idx32;
  UINT tris = 0;
};

bool IsPow2(unsigned v) { return v && !(v & (v - 1)); }
unsigned NextPow2(unsigned v) {
  unsigned p = 1;
  while (p < v) p <<= 1;
  return p;
}

// Halves a premultiplied B, G, R, A picture (box filter; odd sizes keep the
// last row / column).
void Halve(const std::vector<uint8_t>& src, unsigned w, unsigned h, std::vector<uint8_t>& dst, unsigned& dw, unsigned& dh) {
  dw = std::max(1u, w / 2);
  dh = std::max(1u, h / 2);
  dst.resize(static_cast<size_t>(dw) * dh * 4);
  for (unsigned y = 0; y < dh; ++y) {
    const unsigned y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
    for (unsigned x = 0; x < dw; ++x) {
      const unsigned x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
      for (int k = 0; k < 4; ++k) {
        const unsigned s = src[(static_cast<size_t>(y0) * w + x0) * 4 + k] + src[(static_cast<size_t>(y0) * w + x1) * 4 + k] +
                           src[(static_cast<size_t>(y1) * w + x0) * 4 + k] + src[(static_cast<size_t>(y1) * w + x1) * 4 + k];
        dst[(static_cast<size_t>(y) * dw + x) * 4 + k] = static_cast<uint8_t>((s + 2) / 4);
      }
    }
  }
}

// Bilinear resize (cards that only take power-of-two textures).
void Resize(const std::vector<uint8_t>& src, unsigned w, unsigned h, std::vector<uint8_t>& dst, unsigned dw, unsigned dh) {
  dst.resize(static_cast<size_t>(dw) * dh * 4);
  for (unsigned y = 0; y < dh; ++y) {
    const float fy = std::max(0.0f, (y + 0.5f) * h / dh - 0.5f);
    const unsigned y0 = std::min(h - 1, static_cast<unsigned>(fy)), y1 = std::min(h - 1, y0 + 1);
    const float ty = fy - y0;
    for (unsigned x = 0; x < dw; ++x) {
      const float fx = std::max(0.0f, (x + 0.5f) * w / dw - 0.5f);
      const unsigned x0 = std::min(w - 1, static_cast<unsigned>(fx)), x1 = std::min(w - 1, x0 + 1);
      const float tx = fx - x0;
      for (int k = 0; k < 4; ++k) {
        const float a = src[(static_cast<size_t>(y0) * w + x0) * 4 + k], b = src[(static_cast<size_t>(y0) * w + x1) * 4 + k];
        const float c = src[(static_cast<size_t>(y1) * w + x0) * 4 + k], d = src[(static_cast<size_t>(y1) * w + x1) * 4 + k];
        const float v = (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
        dst[(static_cast<size_t>(y) * dw + x) * 4 + k] = static_cast<uint8_t>(v + 0.5f);
      }
    }
  }
}

}  // namespace

struct RenderD3D9::Texture {
  std::vector<uint8_t> px;  // premultiplied B, G, R, A (kept for a new device)
  unsigned w = 0, h = 0;
  IDirect3DTexture9* tex = nullptr;
  IDirect3DDevice9* dev = nullptr;  // the device tex was made on
  bool mips = false;
  bool failed = false;  // could not be made on dev: not tried again on it
  std::string what;
};

RenderD3D9::~RenderD3D9() {
  for (Texture* t : textures_) {
    dd::SafeRelease(t->tex);
    delete t;
  }
}

bool RenderD3D9::MakeTexture(Texture* t) {
  IDirect3DDevice9* dev = dd::B().dev9;
  if (t->dev == dev && (t->tex || t->failed)) return t->tex != nullptr;
  dd::SafeRelease(t->tex);
  t->dev = dev;
  t->failed = true;
  if (!dev) return false;
  const D3DCAPS9& caps = dd::Caps9();
  // Picture sizes the card takes: power-of-two only cards get a resized copy;
  // "conditional" ones take any size without mipmaps.
  const bool pow2_only = (caps.TextureCaps & D3DPTEXTURECAPS_POW2) && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL);
  const bool pow2 = IsPow2(t->w) && IsPow2(t->h);
  std::vector<uint8_t> level;
  unsigned w = t->w, h = t->h;
  const std::vector<uint8_t>* src = &t->px;
  if (!pow2 && pow2_only) {
    w = NextPow2(t->w);
    h = NextPow2(t->h);
    Resize(t->px, t->w, t->h, level, w, h);
    src = &level;
  }
  const unsigned max_w = caps.MaxTextureWidth ? caps.MaxTextureWidth : 2048, max_h = caps.MaxTextureHeight ? caps.MaxTextureHeight : 2048;
  if (w > max_w || h > max_h) {
    const float s = std::min(static_cast<float>(max_w) / w, static_cast<float>(max_h) / h);
    unsigned nw = std::max(1u, static_cast<unsigned>(w * s)), nh = std::max(1u, static_cast<unsigned>(h * s));
    if (pow2_only) {
      while (nw & (nw - 1)) nw &= nw - 1;
      while (nh & (nh - 1)) nh &= nh - 1;
    }
    std::vector<uint8_t> smaller;
    Resize(*src, w, h, smaller, nw, nh);
    level.swap(smaller);
    src = &level;
    dd::Log("MenuMaX: picture %s %ux%u is bigger than the card takes; made %ux%u", t->what.c_str(), w, h, nw, nh);
    w = nw;
    h = nh;
  }
  // Without the POW2 cap any size takes mipmaps; with it, only powers of two.
  t->mips = (IsPow2(w) && IsPow2(h)) || !(caps.TextureCaps & D3DPTEXTURECAPS_POW2);
  UINT levels = 1;
  if (t->mips)
    for (unsigned m = std::max(w, h); m > 1; m >>= 1) ++levels;
  HRESULT hr = dev->CreateTexture(w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t->tex, nullptr);
  if (FAILED(hr) || !t->tex) {
    t->tex = nullptr;
    ++stats_.texture_failures;
    dd::Log("MenuMaX: Direct3D 9 texture %ux%u (%u levels) for %s could not be made: %08lx", w, h, levels, t->what.c_str(), hr);
    return false;
  }
  // Level 0, then each smaller level from the one before (mipmaps keep
  // pictures made bigger than they are shown smooth).
  std::vector<uint8_t> cur, next;
  if (src != &level) cur = *src;
  else cur.swap(level);
  for (UINT l = 0; l < levels; ++l) {
    D3DLOCKED_RECT lr;
    if (FAILED(t->tex->LockRect(l, &lr, nullptr, 0))) {
      dd::LogOnce("MenuMaX: Direct3D 9 texture level could not be locked");
      break;
    }
    for (unsigned y = 0; y < h; ++y)
      std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &cur[static_cast<size_t>(y) * w * 4], w * 4);
    t->tex->UnlockRect(l);
    if (l + 1 < levels) {
      unsigned nw, nh;
      Halve(cur, w, h, next, nw, nh);
      cur.swap(next);
      w = nw;
      h = nh;
    }
  }
  t->failed = false;
  return true;
}

RenderD3D9::Texture* RenderD3D9::NewTexture(std::vector<uint8_t>& bgra, unsigned w, unsigned h, const char* what) {
  auto* t = new Texture;
  t->px.swap(bgra);
  t->w = w;
  t->h = h;
  t->what = what;
  if (!MakeTexture(t)) {
    // Only a missing device is worth another try.
    if (dd::B().dev9) {
      delete t;
      return nullptr;
    }
  }
  textures_.push_back(t);
  ++stats_.textures;
  return t;
}

void RenderD3D9::DeviceGone() {
  unsigned n = 0;
  for (Texture* t : textures_) {
    if (t->tex) ++n;
    dd::SafeRelease(t->tex);
    t->dev = nullptr;
    t->failed = false;
  }
  if (n) dd::Log("MenuMaX: Direct3D 9 device going away; %u textures let go (made again when next used)", n);
}

void RenderD3D9::Begin(IDirect3DSurface9* target, unsigned w, unsigned h) {
  IDirect3DDevice9* d = dd::B().dev9;
  drawing_ = d && target && w && h;
  if (!drawing_) return;
  if (!logged_caps_) {
    logged_caps_ = true;
    const D3DCAPS9& c = dd::Caps9();
    dd::Log("MenuMaX: Direct3D 9 drawing into %ux%u; card: max texture %lux%lu, pow2 %d, nonpow2 conditional %d, "
            "max vertex index %lu, scissor %d",
            w, h, c.MaxTextureWidth, c.MaxTextureHeight, (c.TextureCaps & D3DPTEXTURECAPS_POW2) != 0,
            (c.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) != 0, c.MaxVertexIndex,
            (c.RasterCaps & D3DPRASTERCAPS_SCISSORTEST) != 0);
  }
  width_ = w;
  height_ = h;
  has_transform_ = false;
  transform_ = Rml::Matrix4f::ProjectOrtho(0, static_cast<float>(w), static_cast<float>(h), 0, -10000, 10000);
  dd::EndScene9();
  dd::SetTarget9(target, nullptr);
  dd::SetViewport9(D3DVIEWPORT9{0, 0, w, h, 0.0f, 1.0f});
  // Everything RmlUi hands over is premultiplied.
  dd::RS9(D3DRS_ZENABLE, FALSE);
  dd::RS9(D3DRS_ZWRITEENABLE, FALSE);
  dd::RS9(D3DRS_ALPHABLENDENABLE, TRUE);
  dd::RS9(D3DRS_SRCBLEND, D3DBLEND_ONE);
  dd::RS9(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dd::RS9(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  dd::RS9(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dd::RS9(D3DRS_ALPHATESTENABLE, FALSE);
  dd::RS9(D3DRS_CULLMODE, D3DCULL_NONE);
  dd::RS9(D3DRS_FOGENABLE, FALSE);
  dd::RS9(D3DRS_LIGHTING, FALSE);
  dd::RS9(D3DRS_SPECULARENABLE, FALSE);
  dd::RS9(D3DRS_FILLMODE, D3DFILL_SOLID);
  dd::RS9(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  dd::RS9(D3DRS_STENCILENABLE, FALSE);
  dd::RS9(D3DRS_SCISSORTESTENABLE, FALSE);
  dd::RS9(D3DRS_CLIPPING, TRUE);
  dd::RS9(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  dd::RS9(D3DRS_COLORWRITEENABLE, 0xf);
  dd::RS9(D3DRS_SRGBWRITEENABLE, FALSE);
  dd::RS9(D3DRS_DEPTHBIAS, 0);
  dd::RS9(D3DRS_SLOPESCALEDEPTHBIAS, 0);
  dd::TSS9(0, D3DTSS_TEXCOORDINDEX, 0);
  dd::TSS9(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  dd::TSS9(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dd::TSS9(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dd::SS9(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dd::SS9(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dd::SS9(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dd::SS9(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dd::SS9(0, D3DSAMP_MAXMIPLEVEL, 0);
  dd::SS9(0, D3DSAMP_MAXANISOTROPY, 1);
  dd::SS9(0, D3DSAMP_MIPMAPLODBIAS, 0);
  dd::SS9(0, D3DSAMP_SRGBTEXTURE, 0);
  d->SetFVF(kFvf);
}

void RenderD3D9::End() {
  if (!drawing_) return;
  dd::RS9(D3DRS_SCISSORTESTENABLE, FALSE);
  dd::Tex9(0, nullptr);
  dd::EndScene9();
  drawing_ = false;
}

Rml::CompiledGeometryHandle RenderD3D9::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
  if (vertices.empty() || indices.size() < 3) return 0;
  auto* g = new Geometry;
  g->verts.assign(vertices.begin(), vertices.end());
  if (vertices.size() <= 0x10000) {
    g->idx16.reserve(indices.size());
    for (int i : indices) g->idx16.push_back(static_cast<uint16_t>(i));
  } else {
    g->idx32.assign(indices.begin(), indices.end());
  }
  g->tris = static_cast<UINT>(indices.size() / 3);
  ++stats_.geometry;
  return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void RenderD3D9::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
  auto* g = reinterpret_cast<Geometry*>(geometry);
  IDirect3DDevice9* d = dd::B().dev9;
  if (!g || !drawing_ || !d) return;
  if (!g->idx32.empty() && dd::Caps9().MaxVertexIndex <= 0xFFFF) {
    dd::LogOnce("MenuMaX: a shape with %u vertices is too big for this card; not drawn", static_cast<unsigned>(g->verts.size()));
    return;
  }
  // Vertices -> screen pixels. Direct3D 9 pixel centres are at whole
  // coordinates, so everything moves half a pixel up and left.
  static std::vector<V9> v;
  v.resize(g->verts.size());
  for (size_t i = 0; i < g->verts.size(); ++i) {
    const Rml::Vertex& s = g->verts[i];
    const Rml::ColourbPremultiplied& c = s.colour;
    V9& o = v[i];
    o.c = (static_cast<DWORD>(c.alpha) << 24) | (static_cast<DWORD>(c.red) << 16) | (static_cast<DWORD>(c.green) << 8) | c.blue;
    o.u = s.tex_coord.x;
    o.v = s.tex_coord.y;
    const float x = s.position.x + translation.x, y = s.position.y + translation.y;
    if (!has_transform_) {
      o.x = x - 0.5f;
      o.y = y - 0.5f;
      o.z = 0;
      o.rhw = 1;
      continue;
    }
    // clip = M * (x, y, 0, 1); M is column-major.
    const float* m = transform_.data();
    const float cx = m[0] * x + m[4] * y + m[12], cy = m[1] * x + m[5] * y + m[13];
    float cw = m[3] * x + m[7] * y + m[15];
    if (cw < 1e-6f) cw = 1e-6f;
    const float rw = 1.0f / cw;
    o.x = (cx * rw * 0.5f + 0.5f) * width_ - 0.5f;
    o.y = (0.5f - cy * rw * 0.5f) * height_ - 0.5f;
    o.z = 0;
    o.rhw = rw;
  }
  auto* t = reinterpret_cast<Texture*>(texture);
  if (t && MakeTexture(t)) {
    dd::Tex9(0, t->tex);
    dd::SS9(0, D3DSAMP_MIPFILTER, t->mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    dd::TSS9(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dd::TSS9(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dd::TSS9(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dd::TSS9(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dd::TSS9(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dd::TSS9(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
  } else {
    if (t) return;  // its picture could not be made: draw nothing, like a missing texture
    dd::Tex9(0, nullptr);
    dd::TSS9(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dd::TSS9(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dd::TSS9(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dd::TSS9(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  }
  d->SetFVF(kFvf);
  dd::BeginScene9();
  const bool i16 = !g->idx16.empty();
  HRESULT hr = d->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(v.size()), g->tris,
                                         i16 ? static_cast<const void*>(g->idx16.data()) : g->idx32.data(),
                                         i16 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, v.data(), sizeof(V9));
  if (FAILED(hr)) dd::LogOnce("MenuMaX: Direct3D 9 draw failed %08lx", hr);
  ++stats_.draws;
}

void RenderD3D9::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) { delete reinterpret_cast<Geometry*>(geometry); }

Rml::TextureHandle RenderD3D9::LoadTexture(Rml::Vector2i& dims, const Rml::String& source) {
  std::vector<uint8_t> px;
  unsigned w = 0, h = 0;
  if (!LoadPicture(source, px, w, h)) return 0;
  Texture* t = NewTexture(px, w, h, source.c_str());
  if (!t) return 0;
  dims.x = static_cast<int>(w);
  dims.y = static_cast<int>(h);
  return reinterpret_cast<Rml::TextureHandle>(t);
}

Rml::TextureHandle RenderD3D9::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dims) {
  if (dims.x <= 0 || dims.y <= 0 || source.size() < static_cast<size_t>(dims.x) * dims.y * 4) return 0;
  // RmlUi: premultiplied R, G, B, A -> B, G, R, A.
  std::vector<uint8_t> px(source.begin(), source.begin() + static_cast<size_t>(dims.x) * dims.y * 4);
  for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
  Texture* t = NewTexture(px, static_cast<unsigned>(dims.x), static_cast<unsigned>(dims.y), "(generated)");
  return reinterpret_cast<Rml::TextureHandle>(t);
}

void RenderD3D9::ReleaseTexture(Rml::TextureHandle texture) {
  auto* t = reinterpret_cast<Texture*>(texture);
  if (!t) return;
  textures_.erase(std::remove(textures_.begin(), textures_.end(), t), textures_.end());
  if (t->tex && t->dev && t->dev == dd::B().dev9) dd::Tex9(0, nullptr);  // the cached binding may point at it
  dd::SafeRelease(t->tex);
  delete t;
}

void RenderD3D9::EnableScissorRegion(bool enable) {
  if (drawing_) dd::RS9(D3DRS_SCISSORTESTENABLE, enable ? TRUE : FALSE);
}

void RenderD3D9::SetScissorRegion(Rml::Rectanglei region) {
  if (!drawing_) return;
  RECT r{std::max(0, region.Left()), std::max(0, region.Top()), std::min(static_cast<int>(width_), region.Right()),
         std::min(static_cast<int>(height_), region.Bottom())};
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  dd::SetScissor9(r);
  ++stats_.scissors;
}

void RenderD3D9::SetTransform(const Rml::Matrix4f* transform) {
  has_transform_ = transform != nullptr;
  const Rml::Matrix4f projection =
      Rml::Matrix4f::ProjectOrtho(0, static_cast<float>(width_), static_cast<float>(height_), 0, -10000, 10000);
  transform_ = transform ? projection * *transform : projection;
}

}  // namespace ui
