#include "surface.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "directdraw.h"

namespace dd {

namespace {

void* g_surface_vtable = nullptr;

int LowBit(DWORD m) {
  if (!m) return 0;
  int s = 0;
  while (!(m & 1)) {
    m >>= 1;
    ++s;
  }
  return s;
}

int BitCount(DWORD m) {
  int n = 0;
  for (; m; m >>= 1) n += m & 1;
  return n;
}

bool SameLayout(const PixelLayout& a, const PixelLayout& b) {
  return a.bpp == b.bpp && a.palette == b.palette && a.depth == b.depth &&
         std::memcmp(a.mask, b.mask, sizeof(a.mask)) == 0;
}

DWORD ReadPx(const uint8_t* p, UINT bpp) {
  switch (bpp) {
    case 8: return p[0];
    case 16: return *reinterpret_cast<const uint16_t*>(p);
    case 24: return p[0] | (p[1] << 8) | (p[2] << 16);
    default: return *reinterpret_cast<const uint32_t*>(p);
  }
}

void WritePx(uint8_t* p, UINT bpp, DWORD v) {
  switch (bpp) {
    case 8: p[0] = static_cast<uint8_t>(v); break;
    case 16: *reinterpret_cast<uint16_t*>(p) = static_cast<uint16_t>(v); break;
    case 24:
      p[0] = static_cast<uint8_t>(v);
      p[1] = static_cast<uint8_t>(v >> 8);
      p[2] = static_cast<uint8_t>(v >> 16);
      break;
    default: *reinterpret_cast<uint32_t*>(p) = v; break;
  }
}

// Clip a (possibly stretched) blit to both surfaces, keeping the scale.
bool ClipBlt(RECT& dr, RECT& sr, LONG dw, LONG dh, LONG sw, LONG sh) {
  if (dr.right <= dr.left || dr.bottom <= dr.top || sr.right <= sr.left || sr.bottom <= sr.top)
    return false;
  double kx = static_cast<double>(sr.right - sr.left) / (dr.right - dr.left);
  double ky = static_cast<double>(sr.bottom - sr.top) / (dr.bottom - dr.top);
  auto clip = [](LONG& a0, LONG& a1, LONG& b0, LONG& b1, LONG lo, LONG hi, double k) {
    if (a0 < lo) {
      b0 += static_cast<LONG>((lo - a0) * k + 0.5);
      a0 = lo;
    }
    if (a1 > hi) {
      b1 -= static_cast<LONG>((a1 - hi) * k + 0.5);
      a1 = hi;
    }
  };
  clip(dr.left, dr.right, sr.left, sr.right, 0, dw, kx);
  clip(dr.top, dr.bottom, sr.top, sr.bottom, 0, dh, ky);
  clip(sr.left, sr.right, dr.left, dr.right, 0, sw, 1.0 / kx);
  clip(sr.top, sr.bottom, dr.top, dr.bottom, 0, sh, 1.0 / ky);
  return dr.right > dr.left && dr.bottom > dr.top && sr.right > sr.left && sr.bottom > sr.top;
}

}  // namespace

// ---------------------------------------------------------------- PixelLayout

PixelLayout PixelLayout::From(const DDPIXELFORMAT& pf) {
  PixelLayout l;
  if (pf.dwFlags & DDPF_ZBUFFER) {
    l.depth = true;
    l.bpp = pf.dwZBufferBitDepth ? pf.dwZBufferBitDepth : 16;
    return l;
  }
  if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
    l.palette = true;
    l.bpp = 8;
    return l;
  }
  l.bpp = pf.dwRGBBitCount ? pf.dwRGBBitCount : 16;
  l.mask[0] = pf.dwRBitMask;
  l.mask[1] = pf.dwGBitMask;
  l.mask[2] = pf.dwBBitMask;
  l.mask[3] = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0;
  for (int i = 0; i < 4; ++i) {
    l.shift[i] = LowBit(l.mask[i]);
    l.bits[i] = BitCount(l.mask[i]);
  }
  return l;
}

namespace {
// expand[b][v]: a b-bit channel value widened to 8 bits; compress[b][c]: back.
struct ChannelTables {
  uint8_t expand[9][256];
  uint8_t compress[9][256];
  ChannelTables() {
    for (int b = 1; b <= 8; ++b) {
      unsigned max = (1u << b) - 1;
      for (unsigned v = 0; v < 256; ++v) {
        expand[b][v] = static_cast<uint8_t>(v <= max ? (v * 255 + max / 2) / max : 255);
        compress[b][v] = static_cast<uint8_t>((v * max + 127) / 255);
      }
    }
  }
};
const ChannelTables& Tables() {
  static ChannelTables t;
  return t;
}
}  // namespace

void PixelLayout::ToBGRA(DWORD px, uint8_t out[4]) const {
  static const int order[4] = {2, 1, 0, 3};  // r g b a -> B G R A slots
  const ChannelTables& t = Tables();
  for (int c = 0; c < 4; ++c) {
    uint8_t v;
    if (!bits[c]) {
      v = c == 3 ? 255 : 0;
    } else {
      DWORD raw = (px & mask[c]) >> shift[c];
      v = bits[c] >= 8 ? static_cast<uint8_t>(raw >> (bits[c] - 8)) : t.expand[bits[c]][raw];
    }
    out[order[c]] = v;
  }
}

DWORD PixelLayout::FromBGRA(const uint8_t in[4]) const {
  static const int order[4] = {2, 1, 0, 3};
  const ChannelTables& t = Tables();
  DWORD px = 0;
  for (int c = 0; c < 4; ++c) {
    if (!bits[c]) continue;
    DWORD v = in[order[c]];
    DWORD raw = bits[c] >= 8 ? (v << (bits[c] - 8)) : t.compress[bits[c]][v];
    px |= (raw << shift[c]) & mask[c];
  }
  return px;
}

DDPIXELFORMAT MakeRGBFormat(UINT bpp, DWORD r, DWORD g, DWORD b, DWORD a) {
  DDPIXELFORMAT pf{};
  pf.dwSize = sizeof(pf);
  pf.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
  pf.dwRGBBitCount = bpp;
  pf.dwRBitMask = r;
  pf.dwGBitMask = g;
  pf.dwBBitMask = b;
  pf.dwRGBAlphaBitMask = a;
  return pf;
}

// -------------------------------------------------------------------- Surface

Surface::Surface(DirectDraw* dd, const DDSURFACEDESC2& desc, Surface* root)
    : dd_(dd), root_(root), desc_(desc) {
  if (!g_surface_vtable) g_surface_vtable = *reinterpret_cast<void**>(this);
  desc_.dwSize = sizeof(DDSURFACEDESC2);
  DWORD caps = desc_.ddsCaps.dwCaps;
  if (caps & DDSCAPS_PRIMARYSURFACE || !(desc_.dwFlags & DDSD_WIDTH)) {
    if (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
      desc_.dwWidth = dd->ModeWidth();
      desc_.dwHeight = dd->ModeHeight();
    }
  }
  if (!desc_.dwWidth) desc_.dwWidth = 1;
  if (!desc_.dwHeight) desc_.dwHeight = 1;
  if (!(desc_.dwFlags & DDSD_PIXELFORMAT)) {
    if (caps & DDSCAPS_ZBUFFER) {
      desc_.ddpfPixelFormat = DDPIXELFORMAT{};
      desc_.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
      desc_.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
      desc_.ddpfPixelFormat.dwZBufferBitDepth = 16;
      desc_.ddpfPixelFormat.dwZBitMask = 0xffff;
    } else {
      desc_.ddpfPixelFormat = dd->ModeFormat();
    }
  }
  layout_ = PixelLayout::From(desc_.ddpfPixelFormat);
  if (layout_.depth) desc_.ddsCaps.dwCaps |= DDSCAPS_ZBUFFER;
  desc_.dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
  desc_.lPitch = static_cast<LONG>((desc_.dwWidth * layout_.bpp / 8 + 3) & ~3u);
  desc_.lpSurface = nullptr;
  if (desc_.dwFlags & DDSD_CKSRCBLT) {
    has_key_ = true;
    key_ = desc_.ddckCKSrcBlt;
  }
  AllocateMemory();
}

Surface::~Surface() {
  if (dc_) {
    SelectObject(dc_, old_bitmap_);
    DeleteDC(dc_);
  }
  if (dib_) DeleteObject(dib_);
  for (Surface* m : mips_) delete m;
  Surface* b = back_;
  if (!root_) {
    // The root owns the whole flip chain.
    while (b) {
      Surface* next = b->back_;
      b->back_ = nullptr;
      delete b;
      b = next;
    }
  }
  SafeRelease(zbuffer_);
  SafeRelease(palette_);
  SafeRelease(clipper_);
  gpu_.Reset();
  if (!root_ && dd_ && DirectDraw::Current() == dd_) dd_->ForgetSurface(this);
}

void Surface::AllocateMemory() {
  mem_.assign(static_cast<size_t>(desc_.lPitch) * desc_.dwHeight, 0);
}

HRESULT Surface::Create(DirectDraw* dd, const DDSURFACEDESC2& in, Surface** out) {
  DDSURFACEDESC2 desc = in;
  DWORD caps = desc.ddsCaps.dwCaps;
  if (caps & DDSCAPS_PRIMARYSURFACE) {
    desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY |
                           DDSCAPS_LOCALVIDMEM;
    desc.dwFlags &= ~(DDSD_WIDTH | DDSD_HEIGHT);
  } else if (!(caps & DDSCAPS_SYSTEMMEMORY)) {
    desc.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
  }
  Surface* root = new Surface(dd, desc, nullptr);

  if ((caps & DDSCAPS_FLIP) && (desc.dwFlags & DDSD_BACKBUFFERCOUNT) && desc.dwBackBufferCount) {
    DDSURFACEDESC2 bd = root->desc_;
    bd.ddsCaps.dwCaps &= ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE);
    bd.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
    bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
    bd.dwBackBufferCount = 0;
    Surface* prev = root;
    for (DWORD i = 0; i < desc.dwBackBufferCount; ++i) {
      Surface* b = new Surface(dd, bd, root);
      if (i > 0) b->desc_.ddsCaps.dwCaps &= ~DDSCAPS_BACKBUFFER;
      prev->back_ = b;
      prev = b;
    }
  }

  if ((caps & DDSCAPS_MIPMAP) && (caps & DDSCAPS_COMPLEX)) {
    UINT levels = 1;
    if (desc.dwFlags & DDSD_MIPMAPCOUNT) {
      levels = desc.dwMipMapCount;
    } else {
      for (UINT w = root->Width(), h = root->Height(); w > 1 || h > 1; w >>= 1, h >>= 1) ++levels;
    }
    root->desc_.dwFlags |= DDSD_MIPMAPCOUNT;
    root->desc_.dwMipMapCount = levels;
    for (UINT i = 1; i < levels; ++i) {
      DDSURFACEDESC2 md = root->desc_;
      md.dwWidth = std::max<DWORD>(1, root->Width() >> i);
      md.dwHeight = std::max<DWORD>(1, root->Height() >> i);
      md.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
      md.dwMipMapCount = levels - i;
      md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
      Surface* m = new Surface(dd, md, root);
      m->level_ = static_cast<int>(i);
      root->mips_.push_back(m);
    }
  }

  if (caps & DDSCAPS_PRIMARYSURFACE) dd->SetPrimary(root);
  *out = root;
  return DD_OK;
}

Surface* Surface::From(IDirectDrawSurface7* s) {
  if (!s || !g_surface_vtable || *reinterpret_cast<void**>(s) != g_surface_vtable) return nullptr;
  return static_cast<Surface*>(s);
}

Surface* Surface::CopyForeign(DirectDraw* dd, IUnknown* in) {
  // A surface from Windows' own DirectDraw (the video player's): copy its
  // pixels into a temporary surface of ours.
  static const GUID kIID_Surface7 = {0x06675a80, 0x3b9b, 0x11d2,
                                     {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};
  IDirectDrawSurface7* s7 = nullptr;
  if (FAILED(in->QueryInterface(kIID_Surface7, reinterpret_cast<void**>(&s7))) || !s7) {
    LogOnce("Blt from an unknown surface type");
    return nullptr;
  }
  DDSURFACEDESC2 d{};
  d.dwSize = sizeof(d);
  Surface* out = nullptr;
  if (SUCCEEDED(s7->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
    DDSURFACEDESC2 nd{};
    nd.dwSize = sizeof(nd);
    nd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    nd.dwWidth = d.dwWidth;
    nd.dwHeight = d.dwHeight;
    nd.ddpfPixelFormat = d.ddpfPixelFormat;
    nd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    out = new Surface(dd, nd, nullptr);
    size_t row = std::min<size_t>(static_cast<size_t>(out->Pitch()), static_cast<size_t>(d.lPitch));
    for (DWORD y = 0; y < d.dwHeight; ++y)
      std::memcpy(out->mem_.data() + static_cast<size_t>(y) * out->Pitch(),
                  static_cast<const uint8_t*>(d.lpSurface) + static_cast<size_t>(y) * d.lPitch, row);
    s7->Unlock(nullptr);
    ++Counters().foreign;
    unsigned sum = 0;
    for (size_t i = 0; i < out->mem_.size(); i += 4) sum = sum * 31 + out->mem_[i];
    Counters().foreign_sum = sum;
    LogOnce("copying from a system DirectDraw surface %lux%lu (%lu bpp)", d.dwWidth, d.dwHeight,
            d.ddpfPixelFormat.dwRGBBitCount);
  }
  s7->Release();
  return out;
}

bool Surface::IsRenderTargetKind() const {
  return rt_used_ || (desc_.ddsCaps.dwCaps & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE |
                                              DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER |
                                              DDSCAPS_FLIP)) != 0;
}

void Surface::KeyColor(float out[3]) const {
  uint8_t c[4];
  layout_.ToBGRA(key_.dwColorSpaceLowValue, c);
  out[0] = c[2] / 255.0f;
  out[1] = c[1] / 255.0f;
  out[2] = c[0] / 255.0f;
}

void Surface::PaletteBGRA(uint8_t out[256][4]) const {
  PALETTEENTRY pe[256] = {};
  if (palette_) palette_->GetEntries(0, 0, 256, pe);
  for (int i = 0; i < 256; ++i) {
    out[i][0] = pe[i].peBlue;
    out[i][1] = pe[i].peGreen;
    out[i][2] = pe[i].peRed;
    out[i][3] = 255;
  }
}

// ------------------------------------------------------------ CPU/GPU sync

GpuTexture* Surface::Gpu() {
  Surface* o = SyncOwner();
  if (!o->gpu_.tex) {
    if (!Ready()) return &o->gpu_;
    UINT levels = static_cast<UINT>(o->mips_.size()) + 1;
    if (!CreateGpuTexture(o->gpu_, o->Width(), o->Height(), levels, !o->layout_.depth,
                          o->layout_.depth))
      return &o->gpu_;
    if (o->layout_.depth) {
      o->sync_ = kInSync;
    } else if (o->sync_ == kInSync) {
      o->sync_ = kCpuNewer;
    }
  }
  if (o->sync_ == kCpuNewer) {
    o->Upload();
    o->sync_ = kInSync;
  }
  return &o->gpu_;
}

void Surface::MarkGpuWritten() {
  Surface* o = SyncOwner();
  o->sync_ = kGpuNewer;
  ++uniqueness_;
}

void Surface::SyncToCpu(const char* why) {
  Surface* o = SyncOwner();
  if (o->sync_ == kGpuNewer) {
    NoteReadback(why, o->desc_.ddsCaps.dwCaps, o->Width(), o->Height());
    o->Download();
    o->sync_ = kInSync;
  }
}

void Surface::MarkCpuWritten() {
  Surface* o = SyncOwner();
  if (!o->layout_.depth) o->sync_ = kCpuNewer;
  ++uniqueness_;
}

void Surface::Upload() {
  if (!gpu_.tex || layout_.depth) return;
  uint8_t pal[256][4];
  if (layout_.palette) PaletteBGRA(pal);
  std::vector<uint8_t> bgra;
  for (UINT level = 0; level < gpu_.levels; ++level) {
    Surface* s = level == 0 ? this : (level - 1 < mips_.size() ? mips_[level - 1] : nullptr);
    if (!s) break;
    UINT w = s->Width(), h = s->Height(), bytes = layout_.bpp / 8;
    bgra.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
      const uint8_t* src = s->mem_.data() + static_cast<size_t>(y) * s->Pitch();
      uint8_t* dst = bgra.data() + static_cast<size_t>(y) * w * 4;
      if (layout_.palette) {
        for (UINT x = 0; x < w; ++x) std::memcpy(dst + x * 4, pal[src[x]], 4);
      } else if (layout_.bpp == 32 && layout_.mask[0] == 0xff0000 && layout_.mask[2] == 0xff) {
        for (UINT x = 0; x < w; ++x) {
          std::memcpy(dst + x * 4, src + x * 4, 4);
          if (!layout_.mask[3]) dst[x * 4 + 3] = 255;
        }
      } else {
        for (UINT x = 0; x < w; ++x) layout_.ToBGRA(ReadPx(src + x * bytes, layout_.bpp), dst + x * 4);
      }
    }
    B().ctx->UpdateSubresource(gpu_.tex, level, nullptr, bgra.data(), w * 4, 0);
  }
}

void Surface::Download() {
  if (!gpu_.tex || layout_.depth) return;
  ++Counters().downloads;
  if (!mips_.empty()) LogOnce("reading back a mipmapped surface (top level only)");
  Backend& b = B();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = gpu_.width;
  td.Height = gpu_.height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  // One reusable readback texture per size.
  static std::map<uint64_t, ID3D11Texture2D*> cache;
  ID3D11Texture2D*& staging = cache[(static_cast<uint64_t>(td.Width) << 32) | td.Height];
  if (!staging && FAILED(b.device->CreateTexture2D(&td, nullptr, &staging))) {
    staging = nullptr;
    Log("readback texture %ux%u failed", gpu_.width, gpu_.height);
    return;
  }
  b.ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, gpu_.tex, 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(b.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    UINT w = Width(), h = Height(), bytes = layout_.bpp / 8;
    for (UINT y = 0; y < h; ++y) {
      const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
      uint8_t* dst = mem_.data() + static_cast<size_t>(y) * Pitch();
      if (layout_.palette) continue;  // cannot map colours back to indices
      for (UINT x = 0; x < w; ++x) WritePx(dst + x * bytes, layout_.bpp, layout_.FromBGRA(src + x * 4));
    }
    b.ctx->Unmap(staging, 0);
  }
}

void Surface::CopyFrom(Surface* src, const POINT* dst_pt, const RECT* src_rect) {
  // Device::Load: copy every level both chains have.
  Surface* s = src;
  Surface* d = this;
  RECT sr = src_rect ? *src_rect : RECT{0, 0, static_cast<LONG>(src->Width()), static_cast<LONG>(src->Height())};
  POINT dp = dst_pt ? *dst_pt : POINT{sr.left, sr.top};
  for (size_t level = 0; s && d; ++level) {
    RECT dr{dp.x, dp.y, dp.x + (sr.right - sr.left), dp.y + (sr.bottom - sr.top)};
    RECT r = sr;
    if (ClipBlt(dr, r, d->Width(), d->Height(), s->Width(), s->Height())) d->BltCpu(dr, s, r, false, 0);
    s = level < src->mips_.size() ? src->mips_[level] : nullptr;
    d = level < mips_.size() ? mips_[level] : nullptr;
    sr = {sr.left / 2, sr.top / 2, std::max(sr.left / 2 + 1, sr.right / 2), std::max(sr.top / 2 + 1, sr.bottom / 2)};
    dp = {dp.x / 2, dp.y / 2};
  }
  if (src->has_key_) {
    has_key_ = true;
    key_ = src->key_;
  }
}

// ------------------------------------------------------------------ blits

bool Surface::UseGpuFor(Surface* src) {
  if (InMipChain() || layout_.depth || !Ready()) return false;
  if (src && (src->layout_.depth || src->layout_.palette)) return false;
  // Work where the destination's newest copy is, so CPU drawing (GDI text,
  // software blits) and GPU drawing don't copy the whole surface back and
  // forth for every call.
  bool src_gpu = src && src->SyncOwner()->sync_ == kGpuNewer;
  switch (SyncOwner()->sync_) {
    case kGpuNewer: return true;
    case kCpuNewer: return src_gpu;
    default: return IsRenderTargetKind() || src_gpu;
  }
}

void Surface::BltGpu(const RECT& dr, Surface* src, const RECT& sr, bool key, DWORD key_value) {
  GpuTexture* d = Gpu();
  GpuTexture* s = src->Gpu();
  if (!d->rtv || !s->srv) return;
  GpuTexture temp;
  ID3D11ShaderResourceView* srv = s->srv;
  if (s == d) {
    // Same texture on both sides: blit from a copy.
    if (!CreateGpuTexture(temp, s->width, s->height, 1, false, false)) return;
    B().ctx->CopySubresourceRegion(temp.tex, 0, 0, 0, 0, s->tex, 0, nullptr);
    srv = temp.srv;
  }
  BlitParams p;
  p.dst = d->rtv;
  p.dst_w = d->width;
  p.dst_h = d->height;
  p.dst_rect = dr;
  p.src = srv;
  p.src_w = s->width;
  p.src_h = s->height;
  p.src_rect = sr;
  p.color_key = key;
  if (key) {
    uint8_t c[4];
    src->layout_.ToBGRA(key_value, c);
    p.key[0] = c[2] / 255.0f;
    p.key[1] = c[1] / 255.0f;
    p.key[2] = c[0] / 255.0f;
  }
  p.opaque = layout_.mask[3] == 0;
  Blit(p);
  temp.Reset();
  MarkGpuWritten();
}

void Surface::BltCpu(const RECT& dr, Surface* src, const RECT& sr, bool key, DWORD key_value) {
  src->SyncToCpu("blt-src");
  SyncToCpu("blt-dst");
  std::vector<uint8_t> copy;
  const uint8_t* sbase = src->mem_.data();
  LONG spitch = src->Pitch();
  if (src == this) {
    copy = mem_;
    sbase = copy.data();
  }
  UINT sb = src->layout_.bpp / 8, db = layout_.bpp / 8;
  bool same = SameLayout(layout_, src->layout_);
  uint8_t pal[256][4];
  if (src->layout_.palette) src->PaletteBGRA(pal);
  LONG dw = dr.right - dr.left, dh = dr.bottom - dr.top;
  LONG sw = sr.right - sr.left, sh = sr.bottom - sr.top;
  bool stretch = dw != sw || dh != sh;
  for (LONG y = 0; y < dh; ++y) {
    LONG sy = sr.top + (stretch ? static_cast<LONG>(static_cast<int64_t>(y) * sh / dh) : y);
    const uint8_t* srow = sbase + sy * spitch;
    uint8_t* drow = Pixel(dr.left, dr.top + y);
    if (same && !key && !stretch) {
      std::memcpy(drow, srow + sr.left * sb, static_cast<size_t>(dw) * db);
      continue;
    }
    for (LONG x = 0; x < dw; ++x) {
      LONG sx = sr.left + (stretch ? static_cast<LONG>(static_cast<int64_t>(x) * sw / dw) : x);
      DWORD px = ReadPx(srow + sx * sb, src->layout_.bpp);
      if (key && px == key_value) continue;
      if (!same) {
        uint8_t c[4];
        if (src->layout_.palette) {
          std::memcpy(c, pal[px & 0xff], 4);
        } else {
          src->layout_.ToBGRA(px, c);
        }
        px = layout_.FromBGRA(c);
      }
      WritePx(drow + x * db, layout_.bpp, px);
    }
  }
  MarkCpuWritten();
}

void Surface::FillGpu(const RECT& dr, DWORD color) {
  GpuTexture* d = Gpu();
  if (!d->rtv) return;
  uint8_t c[4];
  layout_.ToBGRA(color, c);
  BlitParams p;
  p.dst = d->rtv;
  p.dst_w = d->width;
  p.dst_h = d->height;
  p.dst_rect = dr;
  p.fill[0] = c[2] / 255.0f;
  p.fill[1] = c[1] / 255.0f;
  p.fill[2] = c[0] / 255.0f;
  p.fill[3] = c[3] / 255.0f;
  Blit(p);
  MarkGpuWritten();
}

void Surface::FillCpu(const RECT& dr, DWORD color) {
  SyncToCpu("FillCpu");
  for (LONG y = dr.top; y < dr.bottom; ++y)
    for (LONG x = dr.left; x < dr.right; ++x) WritePx(Pixel(x, y), layout_.bpp, color);
  MarkCpuWritten();
}

void Surface::PresentFront(UINT interval) {
  if (interval == 0) ++Counters().front_writes;
  GpuTexture* t = Gpu();
  if (t->srv) dd::Present(t->srv, Width(), Height(), interval);
}

void Surface::SwapContents(Surface* other) {
  std::swap(gpu_, other->gpu_);
  std::swap(mem_, other->mem_);
  std::swap(sync_, other->sync_);
  ++uniqueness_;
  ++other->uniqueness_;
}

// ------------------------------------------------------------------ IUnknown

HRESULT Surface::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (riid == d3d7::IID_IDirectDrawSurface7 || riid == d3d7::IID_IUnknown_) {
    AddRef();
    *out = static_cast<IDirectDrawSurface7*>(this);
    return S_OK;
  }
  if (riid == d3d7::IID_IDirectDrawGammaControl) {
    AddRef();
    *out = static_cast<IDirectDrawGammaControl*>(new GammaControl(this));
    return S_OK;
  }
  LogOnce("Surface::QueryInterface for unsupported {%08lx-%04x-%04x}", riid.Data1, riid.Data2,
          riid.Data3);
  return E_NOINTERFACE;
}

ULONG Surface::AddRef() {
  if (root_) return root_->AddRef();
  return static_cast<ULONG>(InterlockedIncrement(&refs_));
}

ULONG Surface::Release() {
  if (root_) return root_->Release();
  LONG n = InterlockedDecrement(&refs_);
  if (n == 0) {
    DD_LOCK;
    delete this;
  }
  return static_cast<ULONG>(n);
}

// --------------------------------------------------------- IDirectDrawSurface7

HRESULT Surface::AddAttachedSurface(LPDIRECTDRAWSURFACE7 s) {
  DD_LOCK;
  Surface* z = From(s);
  if (!z || !z->IsDepth()) {
    LogOnce("AddAttachedSurface of a non-Z surface");
    return DDERR_CANNOTATTACHSURFACE;
  }
  if (zbuffer_ == z) return DDERR_SURFACEALREADYATTACHED;
  z->AddRef();
  SafeRelease(zbuffer_);
  zbuffer_ = z;
  return DD_OK;
}

HRESULT Surface::AddOverlayDirtyRect(LPRECT) { return DDERR_UNSUPPORTED; }

HRESULT Surface::Blt(LPRECT dr_in, LPDIRECTDRAWSURFACE7 src_in, LPRECT sr_in, DWORD flags,
                     LPDDBLTFX fx) {
  DD_LOCK;
  RECT full{0, 0, static_cast<LONG>(Width()), static_cast<LONG>(Height())};
  RECT dr = dr_in ? *dr_in : full;
  if (flags & DDBLT_COLORFILL) {
    if (!fx) return DDERR_INVALIDPARAMS;
    RECT none{};
    if (!ClipBlt(dr, none = dr, full.right, full.bottom, full.right, full.bottom)) return DD_OK;
    if (UseGpuFor(nullptr)) {
      FillGpu(dr, fx->dwFillColor);
    } else {
      FillCpu(dr, fx->dwFillColor);
    }
    if (IsVisible()) PresentFront(0);
    return DD_OK;
  }
  if (flags & DDBLT_DEPTHFILL) {
    if (!fx) return DDERR_INVALIDPARAMS;
    GpuTexture* g = Gpu();
    DWORD max = layout_.bpp >= 32 ? 0xffffffffu : ((1u << layout_.bpp) - 1);
    if (g->dsv)
      B().ctx->ClearDepthStencilView(g->dsv, D3D11_CLEAR_DEPTH,
                                     static_cast<float>(static_cast<double>(fx->dwFillDepth) / max), 0);
    return DD_OK;
  }
  Surface* src = From(src_in);
  Surface* foreign = nullptr;
  ++Counters().blts;
  if (!src && src_in) src = foreign = CopyForeign(dd_, src_in);
  if (!src) {
    LogOnce("Blt flags %08lx without a usable source", flags);
    return src_in ? DDERR_INVALIDPARAMS : DD_OK;
  }
  struct Drop {
    Surface* s;
    ~Drop() {
      if (s) s->Release();
    }
  } drop{foreign};
  if (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE | DDBLT_ROP | DDBLT_DDFX | DDBLT_ALPHADEST |
               DDBLT_ALPHASRC | DDBLT_ZBUFFER))
    LogOnce("Blt flags %08lx partly unsupported", flags);
  RECT sr = sr_in ? *sr_in : RECT{0, 0, static_cast<LONG>(src->Width()), static_cast<LONG>(src->Height())};
  if (!ClipBlt(dr, sr, full.right, full.bottom, static_cast<LONG>(src->Width()),
               static_cast<LONG>(src->Height())))
    return DD_OK;
  bool key = false;
  DWORD key_value = 0;
  if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) {
    key = true;
    key_value = fx->ddckSrcColorkey.dwColorSpaceLowValue;
  } else if ((flags & DDBLT_KEYSRC) && src->has_key_) {
    key = true;
    key_value = src->key_.dwColorSpaceLowValue;
  }
  if (UseGpuFor(src)) {
    BltGpu(dr, src, sr, key, key_value);
  } else {
    BltCpu(dr, src, sr, key, key_value);
  }
  if (IsVisible()) PresentFront(0);
  return DD_OK;
}

HRESULT Surface::BltBatch(LPDDBLTBATCH, DWORD, DWORD) {
  LogOnce("BltBatch unsupported");
  return DDERR_UNSUPPORTED;
}

HRESULT Surface::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src_in, LPRECT sr_in, DWORD flags) {
  DD_LOCK;
  Surface* src = From(src_in);
  Surface* foreign = nullptr;
  if (!src && src_in) src = foreign = CopyForeign(dd_, src_in);
  if (!src) return DDERR_INVALIDPARAMS;
  if (foreign) src_in = foreign;
  struct Drop {
    Surface* s;
    ~Drop() {
      if (s) s->Release();
    }
  } drop{foreign};
  RECT sr = sr_in ? *sr_in : RECT{0, 0, static_cast<LONG>(src->Width()), static_cast<LONG>(src->Height())};
  RECT dr{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x) + sr.right - sr.left,
          static_cast<LONG>(y) + sr.bottom - sr.top};
  DWORD bf = DDBLT_WAIT;
  if (flags & DDBLTFAST_SRCCOLORKEY) bf |= DDBLT_KEYSRC;
  if (flags & DDBLTFAST_DESTCOLORKEY) bf |= DDBLT_KEYDEST;
  return Blt(&dr, src_in, &sr, bf, nullptr);
}

HRESULT Surface::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 s) {
  DD_LOCK;
  if (zbuffer_ && (!s || From(s) == zbuffer_)) {
    SafeRelease(zbuffer_);
    return DD_OK;
  }
  return DDERR_SURFACENOTATTACHED;
}

HRESULT Surface::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb) {
  if (!cb) return DDERR_INVALIDPARAMS;
  Surface* list[3] = {back_, nullptr, zbuffer_};
  if (level_ == 0 && !mips_.empty()) list[1] = mips_[0];
  if (level_ > 0 && static_cast<size_t>(level_) < root_->mips_.size()) list[1] = root_->mips_[level_];
  for (Surface* s : list) {
    if (!s) continue;
    s->AddRef();
    DDSURFACEDESC2 d = s->desc_;
    if (cb(s, &d, ctx) == DDENUMRET_CANCEL) break;
  }
  return DD_OK;
}

HRESULT Surface::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) {
  return DDERR_UNSUPPORTED;
}

HRESULT Surface::Flip(LPDIRECTDRAWSURFACE7, DWORD flags) {
  DD_LOCK;
  if (!back_) return DDERR_NOTFLIPPABLE;
  ++Counters().flips;
  Surface* cur = this;
  while (cur->back_) {
    cur->SwapContents(cur->back_);
    cur = cur->back_;
  }
  PresentFront((flags & DDFLIP_NOVSYNC) ? 0 : 1);
  return DD_OK;
}

HRESULT Surface::GetAttachedSurface(LPDDSCAPS2 caps, LPDIRECTDRAWSURFACE7* out) {
  if (!caps || !out) return DDERR_INVALIDPARAMS;
  *out = nullptr;
  Surface* found = nullptr;
  DWORD c = caps->dwCaps;
  if (c & DDSCAPS_ZBUFFER) {
    found = zbuffer_;
  } else if (c & DDSCAPS_MIPMAP) {
    if (level_ == 0 && !mips_.empty()) found = mips_[0];
    if (level_ > 0 && static_cast<size_t>(level_) < root_->mips_.size()) found = root_->mips_[level_];
  } else if (c & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)) {
    found = back_;
  }
  if (!found) return DDERR_NOTFOUND;
  found->AddRef();
  *out = found;
  return DD_OK;
}

HRESULT Surface::GetBltStatus(DWORD) { return DD_OK; }

HRESULT Surface::GetCaps(LPDDSCAPS2 caps) {
  if (!caps) return DDERR_INVALIDPARAMS;
  *caps = desc_.ddsCaps;
  return DD_OK;
}

HRESULT Surface::GetClipper(LPDIRECTDRAWCLIPPER* out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = clipper_;
  if (!clipper_) return DDERR_NOCLIPPERATTACHED;
  clipper_->AddRef();
  return DD_OK;
}

HRESULT Surface::GetColorKey(DWORD flags, LPDDCOLORKEY key) {
  if (!key) return DDERR_INVALIDPARAMS;
  if (!(flags & DDCKEY_SRCBLT) || !has_key_) return DDERR_NOCOLORKEY;
  *key = key_;
  return DD_OK;
}

HRESULT Surface::GetDC(HDC* out) {
  DD_LOCK;
  if (!out) return DDERR_INVALIDPARAMS;
  if (dc_) return DDERR_DCALREADYCREATED;
  if (layout_.depth) return DDERR_CANTCREATEDC;
  SyncToCpu("GetDC");
  struct {
    BITMAPINFOHEADER h;
    DWORD colors[256];
  } bi{};
  bi.h.biSize = sizeof(BITMAPINFOHEADER);
  bi.h.biWidth = static_cast<LONG>(Width());
  bi.h.biHeight = -static_cast<LONG>(Height());
  bi.h.biPlanes = 1;
  bi.h.biBitCount = static_cast<WORD>(layout_.bpp);
  if (layout_.palette) {
    uint8_t pal[256][4];
    PaletteBGRA(pal);
    bi.h.biCompression = BI_RGB;
    bi.h.biClrUsed = 256;
    for (int i = 0; i < 256; ++i) std::memcpy(&bi.colors[i], pal[i], 4);
  } else if (layout_.bpp == 16 || layout_.bpp == 32) {
    bi.h.biCompression = BI_BITFIELDS;
    bi.colors[0] = layout_.mask[0];
    bi.colors[1] = layout_.mask[1];
    bi.colors[2] = layout_.mask[2];
  }
  HDC screen = ::GetDC(nullptr);
  dib_ = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &dib_bits_,
                          nullptr, 0);
  dc_ = CreateCompatibleDC(screen);
  ::ReleaseDC(nullptr, screen);
  if (!dib_ || !dc_) {
    if (dc_) DeleteDC(dc_);
    if (dib_) DeleteObject(dib_);
    dc_ = nullptr;
    dib_ = nullptr;
    return DDERR_CANTCREATEDC;
  }
  std::memcpy(dib_bits_, mem_.data(), mem_.size());
  old_bitmap_ = SelectObject(dc_, dib_);
  *out = dc_;
  return DD_OK;
}

HRESULT Surface::ReleaseDC(HDC dc) {
  DD_LOCK;
  if (!dc_ || dc != dc_) return DDERR_INVALIDPARAMS;
  GdiFlush();
  std::memcpy(mem_.data(), dib_bits_, mem_.size());
  SelectObject(dc_, old_bitmap_);
  DeleteDC(dc_);
  DeleteObject(dib_);
  dc_ = nullptr;
  dib_ = nullptr;
  dib_bits_ = nullptr;
  MarkCpuWritten();
  if (IsVisible()) PresentFront(0);
  return DD_OK;
}

HRESULT Surface::GetFlipStatus(DWORD) { return DD_OK; }
HRESULT Surface::GetOverlayPosition(LPLONG, LPLONG) { return DDERR_NOTAOVERLAYSURFACE; }

HRESULT Surface::GetPalette(LPDIRECTDRAWPALETTE* out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = palette_;
  if (!palette_) return DDERR_NOPALETTEATTACHED;
  palette_->AddRef();
  return DD_OK;
}

HRESULT Surface::GetPixelFormat(LPDDPIXELFORMAT pf) {
  if (!pf) return DDERR_INVALIDPARAMS;
  *pf = desc_.ddpfPixelFormat;
  return DD_OK;
}

HRESULT Surface::GetSurfaceDesc(LPDDSURFACEDESC2 desc) {
  if (!desc) return DDERR_INVALIDPARAMS;
  DWORD size = desc->dwSize;
  if (size != sizeof(DDSURFACEDESC2) && size != sizeof(DDSURFACEDESC)) size = sizeof(DDSURFACEDESC2);
  std::memcpy(desc, &desc_, size);
  desc->dwSize = size;
  return DD_OK;
}

HRESULT Surface::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) { return DDERR_ALREADYINITIALIZED; }
HRESULT Surface::IsLost() { return DD_OK; }

HRESULT Surface::Lock(LPRECT rect, LPDDSURFACEDESC2 desc, DWORD flags, HANDLE) {
  DD_LOCK;
  if (!desc) return DDERR_INVALIDPARAMS;
  if (layout_.depth) LogOnce("Lock of a Z-buffer (contents not kept)");
  ++Counters().locks;
  SyncToCpu("Lock");
  GetSurfaceDesc(desc);
  LONG x = rect ? rect->left : 0, y = rect ? rect->top : 0;
  desc->lpSurface = Pixel(x, y);
  desc->dwFlags |= DDSD_LPSURFACE;
  locked_ = true;
  lock_readonly_ = (flags & DDLOCK_READONLY) != 0;
  return DD_OK;
}

HRESULT Surface::Unlock(LPRECT) {
  DD_LOCK;
  if (!locked_) return DDERR_NOTLOCKED;
  locked_ = false;
  if (!lock_readonly_) {
    MarkCpuWritten();
    if (IsVisible()) PresentFront(0);
  }
  return DD_OK;
}

HRESULT Surface::Restore() { return DD_OK; }

HRESULT Surface::SetClipper(LPDIRECTDRAWCLIPPER c) {
  DD_LOCK;
  if (c) c->AddRef();
  SafeRelease(clipper_);
  clipper_ = c;
  return DD_OK;
}

HRESULT Surface::SetColorKey(DWORD flags, LPDDCOLORKEY key) {
  DD_LOCK;
  if (flags & DDCKEY_SRCBLT) {
    has_key_ = key != nullptr;
    if (key) {
      key_ = *key;
      if (!(flags & DDCKEY_COLORSPACE)) key_.dwColorSpaceHighValue = key_.dwColorSpaceLowValue;
    }
    desc_.ddckCKSrcBlt = key_;
    if (has_key_) {
      desc_.dwFlags |= DDSD_CKSRCBLT;
    } else {
      desc_.dwFlags &= ~DDSD_CKSRCBLT;
    }
    return DD_OK;
  }
  LogOnce("SetColorKey flags %08lx unsupported", flags);
  return DD_OK;
}

HRESULT Surface::SetOverlayPosition(LONG, LONG) { return DDERR_NOTAOVERLAYSURFACE; }

HRESULT Surface::SetPalette(LPDIRECTDRAWPALETTE p) {
  DD_LOCK;
  if (p) p->AddRef();
  SafeRelease(palette_);
  palette_ = p;
  if (layout_.palette) MarkCpuWritten();
  return DD_OK;
}

HRESULT Surface::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX) {
  return DDERR_NOTAOVERLAYSURFACE;
}
HRESULT Surface::UpdateOverlayDisplay(DWORD) { return DDERR_NOTAOVERLAYSURFACE; }
HRESULT Surface::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE7) { return DDERR_NOTAOVERLAYSURFACE; }

HRESULT Surface::GetDDInterface(LPVOID* out) {
  if (!out) return DDERR_INVALIDPARAMS;
  dd_->AddRef();
  *out = static_cast<IDirectDraw7*>(dd_);
  return DD_OK;
}

HRESULT Surface::PageLock(DWORD) { return DD_OK; }
HRESULT Surface::PageUnlock(DWORD) { return DD_OK; }

HRESULT Surface::SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) {
  LogOnce("SetSurfaceDesc unsupported");
  return DDERR_UNSUPPORTED;
}

HRESULT Surface::SetPrivateData(REFGUID, LPVOID, DWORD, DWORD) { return DD_OK; }
HRESULT Surface::GetPrivateData(REFGUID, LPVOID, LPDWORD) { return DDERR_NOTFOUND; }
HRESULT Surface::FreePrivateData(REFGUID) { return DD_OK; }

HRESULT Surface::GetUniquenessValue(LPDWORD v) {
  if (!v) return DDERR_INVALIDPARAMS;
  *v = uniqueness_;
  return DD_OK;
}

HRESULT Surface::ChangeUniquenessValue() {
  ++uniqueness_;
  return DD_OK;
}

HRESULT Surface::SetPriority(DWORD p) {
  priority_ = p;
  return DD_OK;
}

HRESULT Surface::GetPriority(LPDWORD p) {
  if (!p) return DDERR_INVALIDPARAMS;
  *p = priority_;
  return DD_OK;
}

HRESULT Surface::SetLOD(DWORD l) {
  lod_ = l;
  return DD_OK;
}

HRESULT Surface::GetLOD(LPDWORD l) {
  if (!l) return DDERR_INVALIDPARAMS;
  *l = lod_;
  return DD_OK;
}

// -------------------------------------------------------------------- Palette

Palette::Palette(DWORD caps, const PALETTEENTRY* in) : caps_(caps) {
  if (in) std::memcpy(entries, in, sizeof(PALETTEENTRY) * ((caps & DDPCAPS_8BIT) ? 256 : 16));
}

HRESULT Palette::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (riid == IID_IDirectDrawPalette || riid == d3d7::IID_IUnknown_) {
    AddRef();
    *out = this;
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG Palette::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG Palette::Release() {
  LONG n = InterlockedDecrement(&refs_);
  if (!n) delete this;
  return static_cast<ULONG>(n);
}

HRESULT Palette::GetCaps(LPDWORD caps) {
  if (!caps) return DDERR_INVALIDPARAMS;
  *caps = caps_;
  return DD_OK;
}

HRESULT Palette::GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY out) {
  if (!out || base >= 256) return DDERR_INVALIDPARAMS;
  count = std::min<DWORD>(count, 256 - base);
  std::memcpy(out, entries + base, count * sizeof(PALETTEENTRY));
  return DD_OK;
}

HRESULT Palette::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }

HRESULT Palette::SetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY in) {
  if (!in || base >= 256) return DDERR_INVALIDPARAMS;
  count = std::min<DWORD>(count, 256 - base);
  std::memcpy(entries + base, in, count * sizeof(PALETTEENTRY));
  return DD_OK;
}

// -------------------------------------------------------------------- Clipper

HRESULT Clipper::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (riid == IID_IDirectDrawClipper || riid == d3d7::IID_IUnknown_) {
    AddRef();
    *out = this;
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG Clipper::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG Clipper::Release() {
  LONG n = InterlockedDecrement(&refs_);
  if (!n) delete this;
  return static_cast<ULONG>(n);
}

HRESULT Clipper::GetClipList(LPRECT, LPRGNDATA, LPDWORD size) {
  if (size) *size = 0;
  return DDERR_NOCLIPLIST;
}

HRESULT Clipper::GetHWnd(HWND* out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = hwnd_;
  return DD_OK;
}

HRESULT Clipper::Initialize(LPDIRECTDRAW, DWORD) { return DDERR_ALREADYINITIALIZED; }

HRESULT Clipper::IsClipListChanged(BOOL* out) {
  if (out) *out = FALSE;
  return DD_OK;
}

HRESULT Clipper::SetClipList(LPRGNDATA, DWORD) { return DD_OK; }

HRESULT Clipper::SetHWnd(DWORD, HWND h) {
  hwnd_ = h;
  return DD_OK;
}

// --------------------------------------------------------------- GammaControl

HRESULT GammaControl::QueryInterface(REFIID riid, void** out) {
  return owner_->QueryInterface(riid, out);
}

ULONG GammaControl::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG GammaControl::Release() {
  LONG n = InterlockedDecrement(&refs_);
  if (!n) {
    owner_->Release();
    delete this;
  }
  return static_cast<ULONG>(n);
}

HRESULT GammaControl::GetGammaRamp(DWORD, LPDDGAMMARAMP ramp) {
  if (!ramp) return DDERR_INVALIDPARAMS;
  if (!set_) {
    for (int i = 0; i < 256; ++i) ramp_.red[i] = ramp_.green[i] = ramp_.blue[i] = static_cast<WORD>(i * 257);
  }
  *ramp = ramp_;
  return DD_OK;
}

HRESULT GammaControl::SetGammaRamp(DWORD, LPDDGAMMARAMP ramp) {
  if (!ramp) return DDERR_INVALIDPARAMS;
  ramp_ = *ramp;
  set_ = true;
  LogOnce("gamma ramp set (not applied yet)");
  return DD_OK;
}

}  // namespace dd
