#include "../smooth.h"
#include "surface.h"
#include "gdihook.h"
#include "gputext.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "../config.h"
#include "../framerate.h"
#include "directdraw.h"
#include "msaa.h"
#include "texreplace.h"

namespace dd {

namespace {

void* g_surface_vtable = nullptr;
// Every live surface (for a Direct3D 9 device reset).
std::set<Surface*>& AllSurfaces() {
  static std::set<Surface*> s;
  return s;
}

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
  AllSurfaces().insert(this);
}

Surface::~Surface() {
  AllSurfaces().erase(this);
  if (gpu_dc_) {
    ReleaseGpuDC();
  } else if (dc_) {
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
  replaced_.Reset();
  keyed_.Reset();
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
  if (caps & DDSCAPS_TEXTURE) {
    root->tex_name_ = TakePendingTextureName();
    if (!root->tex_name_.empty() && GetConfig().texture_log) {
      static unsigned named = 0;
      if (++named <= 400)
        Log("texture surface %lux%lu named \"%s\"", root->desc_.dwWidth, root->desc_.dwHeight,
            root->tex_name_.c_str());
    }
  }
  if ((caps & DDSCAPS_TEXTURE) && GetConfig().texture_log) {
    // TextureLog: the first few texture surfaces of each depth.
    static unsigned per_bpp[5] = {};
    const UINT bpp = root->layout_.bpp;
    const UINT slot = bpp == 8 ? 0 : bpp == 16 ? 1 : bpp == 24 ? 2 : bpp == 32 ? 3 : 4;
    if (++per_bpp[slot] <= 5)
      Log("texture surface %lux%lu %u bpp masks %08lx/%08lx/%08lx/%08lx caps %08lx (#%u at this depth)",
          root->desc_.dwWidth, root->desc_.dwHeight, bpp, root->layout_.mask[0], root->layout_.mask[1],
          root->layout_.mask[2], root->layout_.mask[3], caps, per_bpp[slot]);
  }

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
  // Whoever asks is about to read or write the texture outside 3D drawing:
  // bring in what was drawn into its multisampled copy.
  MsaaRelease(SyncOwner()->gpu_.Native());
  return GpuForDraw();
}

bool Surface::WantRenderTarget() const {
  if (layout_.depth) return false;
  if (!IsD3D9()) return true;  // D3D11: every colour texture can be drawn into
  // D3D9: render targets live in video memory only (lost on a device reset,
  // uploaded through a staging copy), so plain textures stay managed.
  if (!mips_.empty() || level_ > 0) return false;
  return IsRenderTargetKind() || !(desc_.ddsCaps.dwCaps & DDSCAPS_TEXTURE);
}

GpuTexture* Surface::GpuForDraw() {
  Surface* o = SyncOwner();
  if (o->gpu_.Valid() && IsD3D9() && !o->gpu_.rt && o->WantRenderTarget()) {
    // A texture that has become a render target: remake it as one. A managed
    // texture is never newer than the CPU copy, so nothing is lost.
    Log("surface %ux%u caps %08lx became a render target; recreating its texture", o->Width(),
        o->Height(), o->Caps());
    o->gpu_.Reset();
    if (o->sync_ == kInSync) o->sync_ = kCpuNewer;
  }
  if (!o->gpu_.Valid()) {
    if (!Ready()) return &o->gpu_;
    UINT levels = static_cast<UINT>(o->mips_.size()) + 1;
    if (!CreateGpuTexture(o->gpu_, o->Width(), o->Height(), levels, o->WantRenderTarget(),
                          o->layout_.depth, o->layout_.mask[3] != 0))
      return &o->gpu_;
    if (o->layout_.depth) {
      o->sync_ = kInSync;
    } else if (o->sync_ == kInSync) {
      o->sync_ = kCpuNewer;
    }
  }
  if (o->sync_ == kCpuNewer) {
    MsaaRelease(o->gpu_.Native());
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
  ++o->cpu_version_;
  ++uniqueness_;
}

namespace {
// Fully transparent texels keep whatever colour the artist left there (often
// grey or black). Filtering and the game's own mip levels mix that colour
// into the visible edge, which shows as a grey halo around leaves. Give
// each transparent texel the average colour of its visible neighbours
// (a few passes, so the colour spreads a little way in); alpha is unchanged.
void BleedIntoTransparent(uint8_t* px, UINT w, UINT h) {
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<uint8_t> known(n);
  size_t unknown = 0;
  for (size_t i = 0; i < n; ++i) {
    known[i] = px[i * 4 + 3] != 0;
    if (!known[i]) ++unknown;
  }
  if (!unknown || unknown == n) return;
  std::vector<uint8_t> next;
  for (int pass = 0; pass < 4 && unknown; ++pass) {
    next = known;
    for (UINT y = 0; y < h; ++y) {
      for (UINT x = 0; x < w; ++x) {
        size_t i = static_cast<size_t>(y) * w + x;
        if (known[i]) continue;
        unsigned sum[3] = {}, count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            int nx = static_cast<int>(x) + dx, ny = static_cast<int>(y) + dy;
            if (nx < 0 || ny < 0 || nx >= static_cast<int>(w) || ny >= static_cast<int>(h)) continue;
            size_t j = static_cast<size_t>(ny) * w + nx;
            if (!known[j]) continue;
            for (int c = 0; c < 3; ++c) sum[c] += px[j * 4 + c];
            ++count;
          }
        }
        if (!count) continue;
        for (int c = 0; c < 3; ++c) px[i * 4 + c] = static_cast<uint8_t>(sum[c] / count);
        next[i] = 1;
        --unknown;
      }
    }
    known.swap(next);
  }
}
}  // namespace

bool Surface::LevelBGRA(UINT level, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
  Surface* s = level == 0 ? this : (level - 1 < mips_.size() ? mips_[level - 1] : nullptr);
  if (!s) return false;
  uint8_t pal[256][4];
  if (layout_.palette) PaletteBGRA(pal);
  w = s->Width();
  h = s->Height();
  const UINT bytes = layout_.bpp / 8;
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
  return true;
}

void Surface::Upload() {
  if (!gpu_.Valid() || layout_.depth) return;
  ++uploads_;
  GpuTextOcclude(gpu_.Native(), nullptr, "cpu write");
  std::vector<uint8_t> bgra;
  for (UINT level = 0; level < gpu_.levels; ++level) {
    UINT w = 0, h = 0;
    if (!LevelBGRA(level, bgra, w, h)) break;
    if (level == 0) MatchReplacement(bgra.data(), w, h);
    if (layout_.mask[3] && uploads_ < 4) BleedIntoTransparent(bgra.data(), w, h);
    UploadLevel(gpu_, level, bgra.data(), w, h);
  }
}

const GpuTexture* Surface::KeyedTex(DWORD key) {
  Surface* o = SyncOwner();
  if (o != this || layout_.depth) return nullptr;
  if (sync_ == kGpuNewer) {
    // Only the GPU copy is current: bring it back first (rare).
    LogOnce("colour key on a surface drawn by the GPU (%ux%u caps %08lx): reading it back",
            Width(), Height(), Caps());
    SyncToCpu("colour key");
  }
  const uint64_t stamp = (static_cast<uint64_t>(cpu_version_) << 32) ^ key ^ 0x8000000000000000ull;
  if (keyed_.Valid() && keyed_stamp_ == stamp) return &keyed_;
  keyed_.Reset();
  const UINT levels = static_cast<UINT>(mips_.size()) + 1;
  if (!CreateGpuTexture(keyed_, Width(), Height(), levels, false, false)) return nullptr;
  uint8_t key_bgra[4];
  uint8_t pal[256][4];
  if (layout_.palette) {
    PaletteBGRA(pal);
    std::memcpy(key_bgra, pal[key & 0xff], 4);
  } else {
    layout_.ToBGRA(key, key_bgra);
  }
  std::vector<uint8_t> bgra;
  unsigned keyed = 0;
  for (UINT level = 0; level < levels; ++level) {
    UINT w = 0, h = 0;
    if (!LevelBGRA(level, bgra, w, h)) break;
    for (size_t i = 0; i < bgra.size(); i += 4) {
      if (bgra[i] == key_bgra[0] && bgra[i + 1] == key_bgra[1] && bgra[i + 2] == key_bgra[2]) {
        bgra[i + 3] = 0;
        ++keyed;
      }
    }
    UploadLevel(keyed_, level, bgra.data(), w, h);
  }
  keyed_stamp_ = stamp;
  if (GetConfig().d3d9_log) {
    static unsigned n = 0;
    if (++n <= 40)
      Log("colour-keyed copy %ux%u caps %08lx key %08lx: %u key texels", Width(), Height(), Caps(),
          key, keyed);
  }
  return &keyed_;
}

void Surface::DeviceLost9() {
  unsigned dropped = 0, lost = 0;
  for (Surface* s : AllSurfaces()) {
    s->keyed_.Reset();
    if (s->gpu_.pool_default) {
      if (s->sync_ == kGpuNewer) ++lost;
      s->gpu_.Reset();
      if (!s->layout_.depth) s->sync_ = kCpuNewer;
      ++dropped;
    }
  }
  Log("device reset: dropped %u video-memory surfaces (%u had GPU-only contents)", dropped, lost);
}

void Surface::MatchReplacement(const uint8_t* bgra, UINT w, UINT h) {
  const Config& cfg = GetConfig();
  const bool eligible = (cfg.texture_replace || cfg.texture_dump) &&
                        (desc_.ddsCaps.dwCaps & DDSCAPS_TEXTURE) && !IsRenderTargetKind() &&
                        uploads_ <= 3;  // not video or other streaming textures
  if (!eligible) {
    replaced_.Reset();
    tex_hash_ = 0;
    return;
  }
  const uint64_t hash = TextureHash(bgra, w, h);
  if (hash == tex_hash_) return;  // same picture as last time
  const bool first = tex_hash_ == 0;
  tex_hash_ = hash;
  replaced_.Reset();
  if (!tex_name_.empty() && !first) {
    // Same surface, different pixels: the game reused it for something
    // else, so its name no longer applies.
    Log("texture \"%s\" %ux%u: pixels changed on re-upload, name dropped", tex_name_.c_str(), w, h);
    tex_name_.clear();
  }
  if (!tex_name_.empty()) {
    // Known by name: dumped and replaced by name, at whatever size the game
    // streamed it in. Falls back to the hash below.
    DumpNamedTexture(tex_name_, bgra, w, h);
    if (LoadReplacementByName(tex_name_, replaced_)) return;
  } else {
    DumpTexture(hash, bgra, w, h);
  }
  LoadReplacement(hash, replaced_);
}

const GpuTexture* Surface::SampleTex() {
  GpuTexture* g = Gpu();
  Surface* o = SyncOwner();
  return o->replaced_.Valid() ? &o->replaced_ : g;
}

void Surface::Download() {
  if (!gpu_.Valid() || layout_.depth) return;
  MsaaRelease(gpu_.Native());
  ++Counters().downloads;
  if (!mips_.empty()) LogOnce("reading back a mipmapped surface (top level only)");
  if (layout_.palette) return;  // cannot map colours back to indices
  struct Ctx {
    Surface* s;
  } ctx{this};
  const bool ok = DownloadTop(
      gpu_,
      [](void* c, UINT y, const uint8_t* src) {
        Surface* s = static_cast<Ctx*>(c)->s;
        if (y >= s->Height()) return;
        const UINT bytes = s->layout_.bpp / 8;
        uint8_t* dst = s->mem_.data() + static_cast<size_t>(y) * s->Pitch();
        for (UINT x = 0; x < s->Width(); ++x)
          WritePx(dst + x * bytes, s->layout_.bpp, s->layout_.FromBGRA(src + x * 4));
      },
      &ctx);
  if (!ok) LogOnce("reading back a %ux%u surface failed", Width(), Height());
}

void Surface::InheritName(Surface* src) {
  // The game loads textures into system-memory surfaces and copies them
  // (Device::Load or a whole-surface Blt) into the video-memory surface it
  // draws with, so the name has to follow the copy. Two cases:
  //  - Terrain and other cached textures reuse one staging surface and a pool
  //    of drawn surfaces, so no surface is created for them: the name of the
  //    texture just created waits for its copy (TakeCopyTextureName).
  //  - Otherwise the staging surface's own name is used, but only while it
  //    still holds the picture it had at its first copy.
  // A named copy starts the drawn surface afresh (the next upload is new).
  // Unnamed copies (shadows, every frame) leave the upload count alone so
  // streaming textures stay out of dumping and replacement.
  if (!(desc_.ddsCaps.dwCaps & DDSCAPS_TEXTURE) || src == this) return;
  std::string name = TakeCopyTextureName();
  const char* how = "created";
  if (name.empty() && !src->tex_name_.empty()) {
    const uint64_t h = src->RawHash();
    if (!src->name_src_hash_) src->name_src_hash_ = h;
    if (h == src->name_src_hash_) {
      name = src->tex_name_;
      how = "source";
    } else {
      if (GetConfig().texture_log) {
        static unsigned n = 0;
        if (++n <= 100)
          Log("texture \"%s\": staging surface %p holds another picture now, its name no longer applies",
              src->tex_name_.c_str(), static_cast<void*>(src));
      }
      src->tex_name_.clear();
    }
  }
  if (name.empty() && tex_name_.empty()) return;  // unnamed before and after: nothing changes
  if (GetConfig().texture_log) {
    static unsigned n = 0;
    if (++n <= 600)
      Log("texture copy %lux%lu %p -> %p: name \"%s\" (%s), was \"%s\"", desc_.dwWidth, desc_.dwHeight,
          static_cast<void*>(src), static_cast<void*>(this), name.c_str(), name.empty() ? "none" : how,
          tex_name_.c_str());
  }
  tex_name_ = name;
  tex_hash_ = 0;
  if (!name.empty()) uploads_ = 0;
}

uint64_t Surface::RawHash() const {
  uint64_t h = 1469598103934665603ull;
  const UINT row = Width() * (layout_.bpp ? layout_.bpp / 8 : 1);
  for (UINT y = 0; y < Height(); ++y) {
    const uint8_t* p = mem_.data() + static_cast<size_t>(y) * Pitch();
    if (static_cast<size_t>(y) * Pitch() + row > mem_.size()) break;
    for (UINT x = 0; x < row; ++x) h = (h ^ p[x]) * 1099511628211ull;
  }
  return h | 1;
}

void Surface::CopyFrom(Surface* src, const POINT* dst_pt, const RECT* src_rect) {
  // Device::Load: copy every level both chains have.
  Surface* s = src;
  Surface* d = this;
  RECT sr = src_rect ? *src_rect : RECT{0, 0, static_cast<LONG>(src->Width()), static_cast<LONG>(src->Height())};
  POINT dp = dst_pt ? *dst_pt : POINT{sr.left, sr.top};
  if (dp.x == 0 && dp.y == 0 && sr.left == 0 && sr.top == 0 && sr.right == static_cast<LONG>(Width()) &&
      sr.bottom == static_cast<LONG>(Height()) && src->Width() == Width() && src->Height() == Height())
    InheritName(src);
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
  // D3D9: only render targets can be drawn into on the GPU.
  if (IsD3D9() && !WantRenderTarget()) return false;
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
  const GpuTexture* s = nullptr;
  if (key && IsD3D9()) {
    // No shaders: the colour key becomes alpha in a keyed copy of the source.
    s = src->KeyedTex(key_value);
    d = Gpu();  // KeyedTex may have read the source back
    if (!s) {
      LogOnce("colour-keyed GPU blit without a keyed copy; using the CPU");
      BltCpu(dr, src, sr, key, key_value);
      return;
    }
  } else {
    s = src->Gpu();
  }
  if (!d->CanRender() || !s->CanSample()) {
    LogOnce("GPU blit %ux%u -> %ux%u skipped: destination %s, source %s", s->width, s->height,
            d->width, d->height, d->CanRender() ? "ok" : "not a render target",
            s->CanSample() ? "ok" : "no texture");
    return;
  }
  GpuTexture temp;
  if (s->Native() == d->Native()) {
    // Same texture on both sides: blit from a copy.
    if (!CopyTexture(temp, *s)) return;
    s = &temp;
  }
  BlitParams p;
  p.SetDst(*d);
  p.dst_rect = dr;
  p.SetSrc(*s);
  p.src_rect = sr;
  p.color_key = key;
  p.alpha_key = key && IsD3D9();
  if (key) {
    uint8_t c[4];
    src->layout_.ToBGRA(key_value, c);
    p.key[0] = c[2] / 255.0f;
    p.key[1] = c[1] / 255.0f;
    p.key[2] = c[0] / 255.0f;
  }
  p.opaque = layout_.mask[3] == 0;
  Blit(p);
  if (key)
    GpuTextOccludeSprite(d->Native(), p);
  else
    GpuTextOcclude(d->Native(), &dr, "blt");
  // Text on the source goes with the copy (menus are put together on an
  // offscreen surface, and the cursor's background is patched from one).
  const void* src_native = src->Gpu()->Native();
  if (GpuTextHasText(src_native)) GpuTextCopy(src_native, sr, d->Native(), d->width, d->height, dr, key);
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
  if (!d->CanRender()) return;
  uint8_t c[4];
  layout_.ToBGRA(color, c);
  BlitParams p;
  p.SetDst(*d);
  p.dst_rect = dr;
  p.fill[0] = c[2] / 255.0f;
  p.fill[1] = c[1] / 255.0f;
  p.fill[2] = c[0] / 255.0f;
  p.fill[3] = c[3] / 255.0f;
  Blit(p);
  GpuTextOcclude(d->Native(), &dr, "fill");
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
  if (t->CanSample()) dd::Present(*t, Width(), Height(), interval);
}

void Surface::SwapContents(Surface* other) {
  std::swap(gpu_, other->gpu_);
  std::swap(mem_, other->mem_);
  std::swap(sync_, other->sync_);
  ++cpu_version_;
  ++other->cpu_version_;
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
    ClearDepth(*g, static_cast<float>(static_cast<double>(fx->dwFillDepth) / max));
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
  if (dr.left == 0 && dr.top == 0 && dr.right == full.right && dr.bottom == full.bottom && sr.left == 0 &&
      sr.top == 0 && sr.right == static_cast<LONG>(src->Width()) && sr.bottom == static_cast<LONG>(src->Height()))
    InheritName(src);
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
  if (!back_) return DDERR_NOTFLIPPABLE;
  WaitForFrameSlot();  // frame cap, outside the lock
  DD_LOCK;
  if (!back_) return DDERR_NOTFLIPPABLE;
  ++Counters().flips;
  Surface* cur = this;
  while (cur->back_) {
    cur->SwapContents(cur->back_);
    cur = cur->back_;
  }
  if (flags & DDFLIP_NOVSYNC) LogOnce("game flips with DDFLIP_NOVSYNC");
  // VSync in mcm2fix.ini decides, whatever the game asked for.
  PresentFront(GetConfig().vsync ? 1 : 0);
  SmoothFrameEnd();
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
  if (HDC t = TextDC()) {
    dc_ = t;
    text_dc_ = true;
    *out = t;
    return DD_OK;
  }
  if (GetGpuDC(out)) {
    GdiTrack(*out, nullptr, Width(), Height(), IsD3D9() ? "gpu" : "gpu (dxgi)");
    return DD_OK;
  }
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
  GdiTrack(dc_, nullptr, Width(), Height(), "cpu copy");
  return DD_OK;
}

HRESULT Surface::ReleaseDC(HDC dc) {
  DD_LOCK;
  if (!dc_ || dc != dc_) return DDERR_INVALIDPARAMS;
  GdiUntrack(dc);
  if (text_dc_) {
    GpuTextEndDC(dc);
    text_dc_ = false;
    dc_ = nullptr;
    ++uniqueness_;
    if (IsVisible()) PresentFront(0);
    return DD_OK;
  }
  if (gpu_dc_) {
    ReleaseGpuDC();
    MarkGpuWritten();
    if (IsVisible()) PresentFront(0);
    return DD_OK;
  }
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

// GDI drawing by mcm2fix on the GPU (gputext.cpp), for render targets.
HDC Surface::TextDC() {
  if (!GpuTextActive() || layout_.depth || layout_.palette || has_key_ || !mips_.empty()) return nullptr;
  if (SyncOwner() != this || !IsRenderTargetKind() || !Ready()) return nullptr;
  GpuTexture* g = Gpu();  // uploads a newer CPU copy first
  if (!g->CanRender() || !g->CanSample()) return nullptr;
  return GpuTextBeginDC(*g, Width(), Height());
}

// GDI drawing straight onto the GPU texture (no readback). Used for render
// targets whose newest copy is on the GPU: the game draws its HUD and menu
// text with GDI onto the 3D back buffer every frame.
bool Surface::GetGpuDC(HDC* out) {
  Surface* o = SyncOwner();
  if (o != this || layout_.palette || has_key_ || !mips_.empty()) return false;
  if (!(o->sync_ == kGpuNewer || (o->sync_ == kInSync && IsRenderTargetKind()))) return false;
  if (!Ready()) return false;
  GpuTexture* g = Gpu();
  if (!g->Valid() || !g->gdi) {
    if (IsD3D9() && IsRenderTargetKind())
      LogOnce("GetDC on %ux%u render target: no GDI on the GPU copy (format %u); using the CPU copy",
              Width(), Height(), static_cast<unsigned>(g->fmt9));
    return false;
  }
  HDC dc = nullptr;
  void* handle = nullptr;
  if (!GpuGetDC(*g, &dc, &handle) || !dc) return false;
  gpu_dc_ = true;
  gpu_dc_handle_ = handle;
  dc_ = dc;
  *out = dc;
  return true;
}

void Surface::ReleaseGpuDC() {
  GpuReleaseDC(gpu_, dc_, gpu_dc_handle_);
  gpu_dc_ = false;
  gpu_dc_handle_ = nullptr;
  dc_ = nullptr;
  // GDI leaves alpha undefined; formats without alpha must read as opaque.
  // (D3D9 gives those surfaces a format without alpha.)
  if (!layout_.mask[3] && gpu_.rtv) {
    BlitParams p;
    p.SetDst(gpu_);
    p.dst_rect = {0, 0, static_cast<LONG>(gpu_.width), static_cast<LONG>(gpu_.height)};
    p.alpha_only = true;
    Blit(p);
  }
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
  ++cpu_version_;
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
