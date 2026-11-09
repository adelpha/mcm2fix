// IDirect3DDevice7 drawing on Direct3D 9 (ini Renderer=D3D9).
//
// Direct3D 7's fixed-function pipeline maps almost one to one onto Direct3D
// 9's: texture stage ops and arguments, blend modes, compare functions, fog
// and cull modes keep their numbers. So nothing is emulated with shaders:
// state is translated and the vertices are converted into one common layout
// (the same one the D3D11 renderer uses).
//
// Differences from the D3D11 renderer, and how they are handled:
//  - Colour keys become alpha in a keyed copy of the texture (KeyedTex) plus
//    an alpha test, instead of a shader comparison.
//  - AlphaCut raises the alpha-test reference instead of a shader discard.
//  - Vertices from ProcessVertices are drawn from their object-space
//    positions with the matrix that processed them, so the GPU clips them
//    against the near plane like Direct3D 7 did.
//  - Pixel centres and fog work as in Direct3D 7 already.
#include "backend9.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../config.h"
#include "device.h"
#include "msaa.h"
#include "surface.h"

namespace dd {

namespace {

struct GpuVertex {
  float pos[4];  // x y z rhw (screen space), or x y z + an unused blend weight
  uint32_t diffuse, specular;
  float t0[2], t1[2];
};
static_assert(sizeof(GpuVertex) == 40, "vertex layout");

constexpr DWORD kFvfScreen = kFvf9XYZRHW | kFvf9Diffuse | kFvf9Specular | kFvf9Tex2;
// XYZB1 with vertex blending off: the fourth float is ignored, so screen-space
// and untransformed vertices share one 40-byte layout.
constexpr DWORD kFvfWorld = kFvf9XYZB1 | kFvf9Diffuse | kFvf9Specular | kFvf9Tex2;

DWORD AsDword(float f) {
  DWORD v;
  std::memcpy(&v, &f, 4);
  return v;
}

float AsFloat(DWORD v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

const D3DMATRIX& IdentityMatrix() {
  static D3DMATRIX m = [] {
    D3DMATRIX r{};
    r._11 = r._22 = r._33 = r._44 = 1.0f;
    return r;
  }();
  return m;
}

// Transform cache (SetTransform is not free).
struct Transforms {
  D3DMATRIX m[3];
  bool ok[3] = {};
  unsigned resets = 0;
} g_xf;

void SetXf(int i, const float* m) {
  if (g_xf.resets != ResetCount9()) {  // a device reset put the defaults back
    g_xf = Transforms{};
    g_xf.resets = ResetCount9();
  }
  static const D3DTRANSFORMSTATETYPE types[3] = {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION};
  if (g_xf.ok[i] && std::memcmp(&g_xf.m[i], m, sizeof(D3DMATRIX)) == 0) return;
  std::memcpy(&g_xf.m[i], m, sizeof(D3DMATRIX));
  g_xf.ok[i] = true;
  B().dev9->SetTransform(types[i], &g_xf.m[i]);
}

UINT PrimCount(DWORD prim, DWORD n) {
  switch (prim) {
    case d3d7::D3DPT_POINTLIST: return n;
    case d3d7::D3DPT_LINELIST: return n / 2;
    case d3d7::D3DPT_LINESTRIP: return n > 0 ? n - 1 : 0;
    case d3d7::D3DPT_TRIANGLELIST: return n / 3;
    case d3d7::D3DPT_TRIANGLESTRIP:
    case d3d7::D3DPT_TRIANGLEFAN: return n > 2 ? n - 2 : 0;
    default: return 0;
  }
}

bool UsesTexture(DWORD op, DWORD a1, DWORD a2) {
  if (op == D3DTOP_DISABLE) return false;
  return (a1 & 0xf) == D3DTA_TEXTURE || (a2 & 0xf) == D3DTA_TEXTURE || op == D3DTOP_BLENDTEXTUREALPHA ||
         op == D3DTOP_BLENDTEXTUREALPHAPM;
}

}  // namespace

bool Device::Init9() {
  Log("device: fixed-function Direct3D 9 path");
  g_xf = Transforms{};
  return Ready();
}

bool Device::BindTarget9(bool& has_depth, bool full_clear) {
  if (!rt_ || !Ready() || DeviceLost9()) return false;
  GpuTexture* rt = rt_->GpuForDraw();
  if (!rt->CanRender()) {
    LogOnce("draw into a %ux%u surface that is not a render target (caps %08lx); skipped", rt_->Width(),
            rt_->Height(), rt_->Caps());
    return false;
  }
  IDirect3DSurface9* ds = nullptr;
  if (Surface* z = rt_->ZBuffer()) {
    GpuTexture* zt = z->Gpu();
    if (zt->surf9 && zt->width >= rt->width && zt->height >= rt->height) {
      ds = zt->surf9;
    } else {
      LogOnce("Z-buffer %ux%u not usable with a %ux%u target", zt->width, zt->height, rt->width,
              rt->height);
    }
  }
  has_depth = ds != nullptr;
  MsaaViews mv;
  cur_msaa_ = has_depth && MsaaAcquire(rt, mv, full_clear);
  IDirect3DSurface9* target = rt->surf9;
  if (cur_msaa_) {
    target = mv.rt9;
    ds = mv.ds9;
  } else {
    MsaaRelease(rt->Native());
  }
  IDirect3DDevice9* dev = B().dev9;
  dev->SetRenderTarget(0, target);
  dev->SetDepthStencilSurface(ds);
  cur_rt9_ = target;
  cur_ds9_ = ds;
  cur_rt9_w_ = rt->width;
  cur_rt9_h_ = rt->height;
  return true;
}

HRESULT Device::Clear9(DWORD count, d3d7::D3DRECT* rects, DWORD flags, d3d7::D3DCOLOR color, float z) {
  if (!rt_ || !Ready() || DeviceLost9()) return d3d7::D3D_OK;
  RECT vp{static_cast<LONG>(viewport_.dwX), static_cast<LONG>(viewport_.dwY),
          static_cast<LONG>(viewport_.dwX + viewport_.dwWidth),
          static_cast<LONG>(viewport_.dwY + viewport_.dwHeight)};
  const bool whole = (!count || !rects) && vp.left <= 0 && vp.top <= 0 &&
                     vp.right >= static_cast<LONG>(rt_->Width()) &&
                     vp.bottom >= static_cast<LONG>(rt_->Height());
  bool has_depth = false;
  if (!BindTarget9(has_depth, whole && (flags & d3d7::D3DCLEAR_TARGET))) return d3d7::D3D_OK;
  const bool clear_color = (flags & d3d7::D3DCLEAR_TARGET) != 0;
  const bool clear_depth = (flags & d3d7::D3DCLEAR_ZBUFFER) && has_depth;
  if (!clear_color && !clear_depth) return d3d7::D3D_OK;
  RECT full{0, 0, static_cast<LONG>(cur_rt9_w_), static_cast<LONG>(cur_rt9_h_)};
  IntersectRect(&vp, &vp, &full);
  std::vector<D3DRECT> list;
  if (!count || !rects) {
    list.push_back({vp.left, vp.top, vp.right, vp.bottom});
  } else {
    for (DWORD i = 0; i < count; ++i) {
      RECT r{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2}, o;
      if (IntersectRect(&o, &r, &vp)) list.push_back({o.left, o.top, o.right, o.bottom});
    }
  }
  if (list.empty()) return d3d7::D3D_OK;
  IDirect3DDevice9* dev = B().dev9;
  D3DVIEWPORT9 v{0, 0, cur_rt9_w_, cur_rt9_h_, 0.0f, 1.0f};
  dev->SetViewport(&v);
  RS9(D3DRS_SCISSORTESTENABLE, FALSE);
  RS9(D3DRS_COLORWRITEENABLE, 0xf);
  DWORD f = (clear_color ? kClear9Target : 0) | (clear_depth ? kClear9ZBuffer | kClear9Stencil : 0);
  HRESULT hr = dev->Clear(static_cast<DWORD>(list.size()), list.data(), f, color, z, 0);
  if (FAILED(hr) && clear_depth) {  // a depth format without stencil
    f &= ~kClear9Stencil;
    hr = dev->Clear(static_cast<DWORD>(list.size()), list.data(), f, color, z, 0);
  }
  if (FAILED(hr)) LogOnce("Clear flags %lx failed %08lx", f, hr);
  if (clear_color) {
    if (cur_msaa_) MsaaMarkDrawn();
    rt_->MarkGpuWritten();
  }
  return d3d7::D3D_OK;
}

void Device::ApplyState9(int mode, bool has_depth) {
  const Config& cfg = GetConfig();
  const D3DCAPS9& caps = Caps9();
  const bool log = cfg.d3d9_log && draws9_ < 40;

  // Depth.
  const bool zon = has_depth && rs_[d3d7::RS_ZENABLE] != 0;
  RS9(D3DRS_ZENABLE, zon ? D3DZB_TRUE : D3DZB_FALSE);
  RS9(D3DRS_ZWRITEENABLE, zon && rs_[d3d7::RS_ZWRITEENABLE] ? TRUE : FALSE);
  DWORD zfunc = rs_[d3d7::RS_ZFUNC];
  if (zfunc < 1 || zfunc > 8) zfunc = 4;
  RS9(D3DRS_ZFUNC, zfunc);
  const DWORD bias = rs_[d3d7::RS_ZBIAS] & 0xff;
  if (caps.RasterCaps & D3DPRASTERCAPS_DEPTHBIAS)
    RS9(D3DRS_DEPTHBIAS, AsDword(-static_cast<float>(bias) * 16.0f / 16777216.0f));
  if (caps.RasterCaps & D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS)
    RS9(D3DRS_SLOPESCALEDEPTHBIAS, AsDword(-static_cast<float>(bias) * 0.5f));

  // Blending (D3DBLEND numbers are the same, BOTHSRCALPHA included).
  const bool blend = rs_[d3d7::RS_ALPHABLENDENABLE] != 0;
  RS9(D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
  if (blend) {
    DWORD src = rs_[d3d7::RS_SRCBLEND], dst = rs_[d3d7::RS_DESTBLEND];
    if (src < 1 || src > 13) src = D3DBLEND_ONE;
    if (dst < 1 || dst > 13) dst = D3DBLEND_ZERO;
    RS9(D3DRS_SRCBLEND, src);
    RS9(D3DRS_DESTBLEND, dst);
  }
  RS9(D3DRS_COLORWRITEENABLE, 0xf);

  // Raster.
  const DWORD cull = rs_[d3d7::RS_CULLMODE];
  RS9(D3DRS_CULLMODE, cull >= 1 && cull <= 3 ? cull : static_cast<DWORD>(D3DCULL_CCW));
  const DWORD fill = rs_[d3d7::RS_FILLMODE];
  RS9(D3DRS_FILLMODE, fill >= 1 && fill <= 3 ? fill : static_cast<DWORD>(D3DFILL_SOLID));
  RS9(D3DRS_SHADEMODE, rs_[d3d7::RS_SHADEMODE] == 1 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
  RS9(D3DRS_LIGHTING, FALSE);  // as the D3D11 renderer: vertex colours as given
  RS9(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  RS9(D3DRS_CLIPPING, TRUE);
  RS9(D3DRS_STENCILENABLE, FALSE);
  RS9(D3DRS_SPECULARENABLE, rs_[d3d7::RS_SPECULARENABLE] ? TRUE : FALSE);
  RS9(D3DRS_DITHERENABLE, FALSE);
  RS9(D3DRS_TEXTUREFACTOR, rs_[d3d7::RS_TEXTUREFACTOR]);
  RS9(D3DRS_SCISSORTESTENABLE, TRUE);

  // Fog, per pixel, with the ini FogDistance scale.
  const bool fog = rs_[d3d7::RS_FOGENABLE] != 0;
  RS9(D3DRS_FOGENABLE, fog ? TRUE : FALSE);
  if (fog) {
    DWORD fmode = rs_[d3d7::RS_FOGTABLEMODE] ? rs_[d3d7::RS_FOGTABLEMODE] : rs_[d3d7::RS_FOGVERTEXMODE];
    fmode &= 3;
    const float scale = cfg.fog_scale;
    RS9(D3DRS_FOGCOLOR, rs_[d3d7::RS_FOGCOLOR]);
    RS9(D3DRS_FOGTABLEMODE, fmode);
    RS9(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
    RS9(D3DRS_RANGEFOGENABLE, FALSE);
    RS9(D3DRS_FOGSTART, AsDword(AsFloat(rs_[d3d7::RS_FOGSTART]) * scale));
    RS9(D3DRS_FOGEND, AsDword(AsFloat(rs_[d3d7::RS_FOGEND]) * scale));
    RS9(D3DRS_FOGDENSITY, AsDword(AsFloat(rs_[d3d7::RS_FOGDENSITY]) / scale));
    LogOnce("fog mode %lu/%lu -> table %lu, start %.1f end %.1f, %s draws, w-fog %s", rs_[d3d7::RS_FOGTABLEMODE],
            rs_[d3d7::RS_FOGVERTEXMODE], fmode, AsFloat(rs_[d3d7::RS_FOGSTART]), AsFloat(rs_[d3d7::RS_FOGEND]),
            mode == 1 ? "screen-space" : "3D", (caps.RasterCaps & D3DPRASTERCAPS_WFOG) ? "yes" : "no");
  }

  // Alpha test, with AlphaCut for blended cut-outs that write depth (see
  // the D3D11 renderer) and the colour key.
  bool atest = rs_[d3d7::RS_ALPHATESTENABLE] != 0;
  DWORD afunc = rs_[d3d7::RS_ALPHAFUNC];
  if (afunc < 1 || afunc > 8) afunc = D3DCMP_ALWAYS;
  DWORD aref = rs_[d3d7::RS_ALPHAREF] & 0xff;
  if (blend && rs_[d3d7::RS_ZWRITEENABLE] && rs_[d3d7::RS_ZENABLE] &&
      (rs_[d3d7::RS_SRCBLEND] == 5 || rs_[d3d7::RS_SRCBLEND] == 12) && atest &&
      (afunc == D3DCMP_GREATER || afunc == D3DCMP_GREATEREQUAL) && cfg.alpha_cut > 0) {
    DWORD lo = afunc == D3DCMP_GREATER ? aref + 1 : aref;
    DWORD cut = static_cast<DWORD>(std::min(cfg.alpha_cut, 255));
    aref = std::max(lo, cut);
    afunc = D3DCMP_GREATEREQUAL;
  }

  // Texture stages.
  bool keyed = false;
  for (DWORD s = 0; s < 2; ++s) {
    const DWORD* t = tss_[s];
    DWORD cop = t[d3d7::TSS_COLOROP], aop = t[d3d7::TSS_ALPHAOP];
    if (cop < 1 || cop > 26) {
      LogOnce("texture op %lu not supported; disabled", cop);
      cop = D3DTOP_DISABLE;
    }
    if (aop < 1 || aop > 26) aop = D3DTOP_DISABLE;
    IDirect3DBaseTexture9* tex = nullptr;
    if (cop != D3DTOP_DISABLE && tex_[s]) {
      if (tex_[s] == rt_) {
        LogOnce("texture is the current render target; ignored");
      } else if (s == 0 && rs_[d3d7::RS_COLORKEYENABLE] && tex_[s]->HasColorKey()) {
        DDCOLORKEY k{};
        tex_[s]->GetColorKey(DDCKEY_SRCBLT, &k);
        if (const GpuTexture* kt = tex_[s]->KeyedTex(k.dwColorSpaceLowValue)) {
          tex = kt->tex9;
          keyed = true;
        }
      } else {
        tex = tex_[s]->SampleTex()->tex9;
      }
    }
    if (!tex && UsesTexture(cop, t[d3d7::TSS_COLORARG1], t[d3d7::TSS_COLORARG2])) tex = WhiteTex9();
    if (!tex && UsesTexture(aop, t[d3d7::TSS_ALPHAARG1], t[d3d7::TSS_ALPHAARG2])) tex = WhiteTex9();
    Tex9(s, tex);
    TSS9(s, D3DTSS_COLOROP, cop);
    TSS9(s, D3DTSS_COLORARG1, t[d3d7::TSS_COLORARG1]);
    TSS9(s, D3DTSS_COLORARG2, t[d3d7::TSS_COLORARG2]);
    TSS9(s, D3DTSS_ALPHAOP, cop == D3DTOP_DISABLE ? static_cast<DWORD>(D3DTOP_DISABLE) : aop);
    TSS9(s, D3DTSS_ALPHAARG1, t[d3d7::TSS_ALPHAARG1]);
    TSS9(s, D3DTSS_ALPHAARG2, t[d3d7::TSS_ALPHAARG2]);
    TSS9(s, D3DTSS_TEXCOORDINDEX, s);  // sets were remapped into t0 / t1
    TSS9(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    if (s == 0 && keyed && !UsesTexture(aop, t[d3d7::TSS_ALPHAARG1], t[d3d7::TSS_ALPHAARG2]))
      LogOnce("colour key with alpha op %lu (args %lx/%lx) that ignores the texture: key has no effect",
              aop, t[d3d7::TSS_ALPHAARG1], t[d3d7::TSS_ALPHAARG2]);

    // Sampler.
    const DWORD mag = t[d3d7::TSS_MAGFILTER] & 7, min = t[d3d7::TSS_MINFILTER] & 7, mip = t[d3d7::TSS_MIPFILTER] & 3;
    DWORD magf = mag == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    DWORD minf = min == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    DWORD aniso = 1;
    if (min == 3 || mag == 5) aniso = std::min<DWORD>(16, caps.MaxAnisotropy);
    if (cfg.anisotropy > 1 && min != 1) aniso = std::min<DWORD>(static_cast<DWORD>(cfg.anisotropy), caps.MaxAnisotropy);
    if (aniso > 1 && (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC)) {
      minf = D3DTEXF_ANISOTROPIC;
      if (caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC && mag != 1) magf = D3DTEXF_ANISOTROPIC;
    } else {
      aniso = 1;
    }
    SS9(s, D3DSAMP_MAGFILTER, magf);
    SS9(s, D3DSAMP_MINFILTER, minf);
    SS9(s, D3DSAMP_MIPFILTER, mip == 3 ? D3DTEXF_LINEAR : mip == 2 ? D3DTEXF_POINT : D3DTEXF_NONE);
    SS9(s, D3DSAMP_MAXANISOTROPY, aniso);
    const DWORD au = t[d3d7::TSS_ADDRESSU] & 7, av = t[d3d7::TSS_ADDRESSV] & 7;
    SS9(s, D3DSAMP_ADDRESSU, au >= 1 && au <= 4 ? au : static_cast<DWORD>(D3DTADDRESS_WRAP));
    SS9(s, D3DSAMP_ADDRESSV, av >= 1 && av <= 4 ? av : static_cast<DWORD>(D3DTADDRESS_WRAP));
    SS9(s, D3DSAMP_MIPMAPLODBIAS, AsDword(AsFloat(t[d3d7::TSS_MIPMAPLODBIAS]) + cfg.mip_bias));
    SS9(s, D3DSAMP_MAXMIPLEVEL, t[d3d7::TSS_MAXMIPLEVEL] & 0xf);
    SS9(s, D3DSAMP_BORDERCOLOR, t[d3d7::TSS_BORDERCOLOR]);
    if (log)
      Log("  stage %lu: tex %p%s colour %lu(%lx,%lx) alpha %lu(%lx,%lx) filter %lu/%lu/%lu aniso %lu address %lu/%lu",
          s, static_cast<void*>(tex), keyed && s == 0 ? " (keyed)" : "", cop, t[d3d7::TSS_COLORARG1],
          t[d3d7::TSS_COLORARG2], aop, t[d3d7::TSS_ALPHAARG1], t[d3d7::TSS_ALPHAARG2], mag, min, mip, aniso, au, av);
    if (cop == D3DTOP_DISABLE) {
      // Disabling a stage ends the cascade.
      if (s == 0) {
        TSS9(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        TSS9(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
      }
      break;
    }
  }
  TSS9(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
  TSS9(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

  if (keyed && !atest) {
    atest = true;
    afunc = D3DCMP_GREATER;
    aref = 0;
  }
  RS9(D3DRS_ALPHATESTENABLE, atest ? TRUE : FALSE);
  if (atest) {
    RS9(D3DRS_ALPHAFUNC, afunc);
    RS9(D3DRS_ALPHAREF, aref);
  }
  if (log)
    Log("  state: z %d write %d func %lu, blend %d %lu/%lu, alpha test %d func %lu ref %lu, cull %lu, "
        "fog %d, specular %d, shade %lu, zbias %lu",
        zon, zon && rs_[d3d7::RS_ZWRITEENABLE] != 0, zfunc, blend, rs_[d3d7::RS_SRCBLEND], rs_[d3d7::RS_DESTBLEND], atest,
        afunc, aref, rs_[d3d7::RS_CULLMODE], fog, rs_[d3d7::RS_SPECULARENABLE] != 0, rs_[d3d7::RS_SHADEMODE], bias);
}

void Device::Submit9(const Draw& d) {
  bool has_depth = false;
  if (!BindTarget9(has_depth, false)) return;
  const FvfLayout& l = d.layout;
  if (!l.stride) return;
  if (d.prim < d3d7::D3DPT_POINTLIST || d.prim > d3d7::D3DPT_TRIANGLEFAN) {
    LogOnce("primitive type %lu unsupported", d.prim);
    return;
  }
  const UINT prims = PrimCount(d.prim, d.idx ? d.nidx : d.count);
  if (!prims) return;

  // Which vertex mode: untransformed, screen space, or (vertices from
  // ProcessVertices) object space with the matrix that processed them.
  int mode = l.tl ? 1 : 0;
  const float* proc_mat = nullptr;
  const VertexBuffer::Processed* clip = nullptr;
  if (l.tl && d.vb && d.vb->HasClipData(d.first, d.count)) {
    clip = d.vb->ClipData();
    const uint32_t id = clip[d.first].mat;
    bool same = true;
    for (DWORD i = 1; i < d.count && same; ++i) same = clip[d.first + i].mat == id;
    proc_mat = same ? d.vb->Matrix(id) : nullptr;
    if (proc_mat) {
      mode = 2;
    } else {
      static unsigned mixed = 0;
      if (++mixed <= 5 || (mixed & 1023) == 0)
        Log("processed vertices with %s matrices drawn in screen space (no near clipping), x%u",
            same ? "stale" : "mixed", mixed);
      clip = nullptr;
    }
  }

  const int set0 = static_cast<int>(tss_[0][d3d7::TSS_TEXCOORDINDEX] & 0xffff);
  const int set1 = static_cast<int>(tss_[1][d3d7::TSS_TEXCOORDINDEX] & 0xffff);
  scratch_.resize(static_cast<size_t>(d.count) * sizeof(GpuVertex));
  auto* out = reinterpret_cast<GpuVertex*>(scratch_.data());
  for (DWORD i = 0; i < d.count; ++i) {
    const uint8_t* s = d.verts + static_cast<size_t>(d.first + i) * l.stride;
    GpuVertex& v = out[i];
    if (clip) {
      std::memcpy(v.pos, clip[d.first + i].obj, 12);
      v.pos[3] = 1.0f;
    } else if (l.tl) {
      std::memcpy(v.pos, s, 16);
    } else {
      std::memcpy(v.pos, s, 12);
      v.pos[3] = 1.0f;
    }
    v.diffuse = 0xffffffff;
    v.specular = 0;
    if (l.diffuse >= 0) std::memcpy(&v.diffuse, s + l.diffuse, 4);
    if (l.specular >= 0) std::memcpy(&v.specular, s + l.specular, 4);
    v.t0[0] = v.t0[1] = v.t1[0] = v.t1[1] = 0;
    if (set0 < l.ntex) std::memcpy(v.t0, s + l.tex[set0], l.tex_dim[set0] >= 2 ? 8 : 4);
    if (set1 < l.ntex) std::memcpy(v.t1, s + l.tex[set1], l.tex_dim[set1] >= 2 ? 8 : 4);
  }

  IDirect3DDevice9* dev = B().dev9;
  // Transforms. Fog decides between w and z from the projection matrix, so
  // screen-space draws keep the game's projection set as well.
  const float* ident = &IdentityMatrix()._11;
  if (mode == 0) {
    SetXf(0, &world_.m[0][0]);
    SetXf(1, &view_.m[0][0]);
    SetXf(2, &proj_.m[0][0]);
  } else if (mode == 2) {
    SetXf(0, ident);
    SetXf(1, ident);
    SetXf(2, proc_mat);
  } else {
    SetXf(2, &proj_.m[0][0]);
  }

  // Viewport (inside the target) and scissor.
  const d3d7::D3DVIEWPORT7& vp7 = mode == 2 ? d.vb->ClipViewport() : viewport_;
  D3DVIEWPORT9 vp{};
  vp.X = std::min<DWORD>(vp7.dwX, cur_rt9_w_ ? cur_rt9_w_ - 1 : 0);
  vp.Y = std::min<DWORD>(vp7.dwY, cur_rt9_h_ ? cur_rt9_h_ - 1 : 0);
  vp.Width = std::max<DWORD>(1, std::min<DWORD>(vp7.dwWidth ? vp7.dwWidth : 1, cur_rt9_w_ - vp.X));
  vp.Height = std::max<DWORD>(1, std::min<DWORD>(vp7.dwHeight ? vp7.dwHeight : 1, cur_rt9_h_ - vp.Y));
  vp.MinZ = vp7.dvMinZ;
  vp.MaxZ = vp7.dvMaxZ > vp7.dvMinZ ? vp7.dvMaxZ : vp7.dvMinZ + 1e-6f;
  dev->SetViewport(&vp);
  RECT sc{static_cast<LONG>(vp.X), static_cast<LONG>(vp.Y), static_cast<LONG>(vp.X + vp.Width),
          static_cast<LONG>(vp.Y + vp.Height)};
  dev->SetScissorRect(&sc);

  const bool log = GetConfig().d3d9_log && draws9_ < 40;
  if (log)
    Log("draw #%u: prim %lu, %lu verts (fvf stride %u%s), %lu indices, mode %d, target %ux%u%s%s, "
        "viewport %lu,%lu %lux%lu z %.2f-%.2f",
        draws9_, d.prim, d.count, l.stride, l.tl ? " xyzrhw" : "", d.idx ? d.nidx : 0, mode, cur_rt9_w_,
        cur_rt9_h_, has_depth ? " +depth" : "", cur_msaa_ ? " msaa" : "", vp.X, vp.Y, vp.Width, vp.Height,
        vp.MinZ, vp.MaxZ);
  ApplyState9(mode, has_depth);

  dev->SetFVF(mode == 1 ? kFvfScreen : kFvfWorld);
  HRESULT hr;
  if (d.idx) {
    hr = dev->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(d.prim), 0, d.count, prims, d.idx,
                                     D3DFMT_INDEX16, scratch_.data(), sizeof(GpuVertex));
  } else {
    hr = dev->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(d.prim), prims, scratch_.data(), sizeof(GpuVertex));
  }
  if (FAILED(hr)) {
    static unsigned fails = 0;
    if (++fails <= 10) Log("draw failed %08lx (prim %lu, %u primitives, mode %d)", hr, d.prim, prims, mode);
  }
  ++draws9_;
  if (cur_msaa_) MsaaMarkDrawn();
  if (has_depth) Note3DDraw();
  rt_->MarkGpuWritten();
  ++Counters().draws;
}

}  // namespace dd
