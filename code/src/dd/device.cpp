#include "../smooth.h"
#include "device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "../config.h"
#include "directdraw.h"
#include "msaa.h"
#include "surface.h"

namespace dd {

using namespace d3d7;

namespace {

// ------------------------------------------------------------------ shaders

const char kShader[] = R"(
#ifdef FLAT
#define SHADE nointerpolation
#else
#define SHADE
#endif
cbuffer VSParams : register(b0) {
  row_major float4x4 wvp;
  float4 vp;    // viewport x y w h (pixels)
  float4 mode;  // x: 0 untransformed, 1 screen space (XYZRHW), 2 clip space
};
cbuffer PSParams : register(b1) {
  int4 scol[2];   // colour op, arg1, arg2, has texture
  int4 salp[2];   // alpha op, arg1, arg2
  float4 tfactor;
  float4 fogcolor;
  float4 fog;     // start end density mode(0 off, 1 exp, 2 exp2, 3 linear)
  float4 atest;   // ref(0-255) func specular colorkey
  float4 key0;    // stage 0 colour key rgb, w = active
  float4 acut;    // x: discard alpha below this (blended draws that write depth)
};
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState smp0 : register(s0);
SamplerState smp1 : register(s1);
SamplerState keysmp : register(s2);
struct VSIn {
  float4 pos : POSITION;
  float4 diff : COLOR0;
  float4 spec : COLOR1;
  float2 t0 : TEXCOORD0;
  float2 t1 : TEXCOORD1;
};
struct VSOut {
  float4 pos : SV_Position;
  SHADE float4 diff : COLOR0;
  SHADE float4 spec : COLOR1;
  float2 t0 : TEXCOORD0;
  float2 t1 : TEXCOORD1;
  float fogw : TEXCOORD2;
};
VSOut vs_main(VSIn i) {
  VSOut o;
  float4 p;
  if (mode.x == 1) {
    float w = i.pos.w > 0 ? 1.0 / i.pos.w : 1.0;
    float2 ndc = float2((i.pos.x - vp.x) / vp.z * 2 - 1, 1 - (i.pos.y - vp.y) / vp.w * 2);
    p = float4(ndc * w, i.pos.z * w, w);
  } else if (mode.x == 2) {
    p = i.pos;
  } else {
    p = mul(float4(i.pos.xyz, 1), wvp);
  }
  // Direct3D 7 samples pixels at integer coordinates, Direct3D 11 at
  // half-pixel centres: shift by half a pixel to match.
  p.x += p.w / vp.z;
  p.y -= p.w / vp.w;
  o.pos = p;
  o.fogw = p.w;
  o.diff = i.diff;
  o.spec = i.spec;
  o.t0 = i.t0;
  o.t1 = i.t1;
  return o;
}
float4 Arg(int a, float4 diff, float4 spec, float4 cur, float4 texc) {
  int sel = a & 15;
  float4 v = cur;
  if (sel == 0) v = diff;
  else if (sel == 2) v = texc;
  else if (sel == 3) v = tfactor;
  else if (sel == 4) v = spec;
  if (a & 0x10) v = 1 - v;
  if (a & 0x20) v = v.aaaa;
  return v;
}
float4 Op(int op, float4 a1, float4 a2, float4 diff, float4 cur, float4 texc) {
  if (op == 2) return a1;
  if (op == 3) return a2;
  if (op == 4) return a1 * a2;
  if (op == 5) return a1 * a2 * 2;
  if (op == 6) return a1 * a2 * 4;
  if (op == 7) return a1 + a2;
  if (op == 8) return a1 + a2 - 0.5;
  if (op == 9) return (a1 + a2 - 0.5) * 2;
  if (op == 10) return a1 - a2;
  if (op == 11) return a1 + a2 - a1 * a2;
  if (op == 12) return lerp(a2, a1, diff.a);
  if (op == 13) return lerp(a2, a1, texc.a);
  if (op == 14) return lerp(a2, a1, tfactor.a);
  if (op == 15) return a1 + a2 * (1 - texc.a);
  if (op == 16) return lerp(a2, a1, cur.a);
  return a1 * a2;
}
float4 Stage(int s, VSOut i, float4 cur, float4 texc) {
  float4 c = Op(scol[s].x, Arg(scol[s].y, i.diff, i.spec, cur, texc),
                Arg(scol[s].z, i.diff, i.spec, cur, texc), i.diff, cur, texc);
  float a = cur.a;
  if (salp[s].x != 1)
    a = Op(salp[s].x, Arg(salp[s].y, i.diff, i.spec, cur, texc),
           Arg(salp[s].z, i.diff, i.spec, cur, texc), i.diff, cur, texc).a;
  return saturate(float4(c.rgb, a));
}
float4 ps_main(VSOut i) : SV_Target {
  float4 white = float4(1, 1, 1, 1);
  float4 tc0 = tex0.Sample(smp0, i.t0);
  float4 tc1 = tex1.Sample(smp1, i.t1);
  if (scol[0].w == 0) tc0 = white;
  if (scol[1].w == 0) tc1 = white;
  float4 cur = i.diff;
  if (scol[0].x != 1) {
    cur = Stage(0, i, cur, tc0);
    if (scol[1].x != 1) cur = Stage(1, i, cur, tc1);
  }
  if (atest.w != 0 && key0.w != 0) {
    float3 k = tex0.Sample(keysmp, i.t0).rgb;
    if (all(abs(k - key0.rgb) < 0.002)) discard;
  }
  if (atest.z != 0) cur.rgb = saturate(cur.rgb + i.spec.rgb);
  if (fog.w != 0) {
    float d = i.fogw;
    float f = 1;
    if (fog.w == 3) f = (fog.y - d) / max(fog.y - fog.x, 1e-6);
    else if (fog.w == 1) f = exp(-fog.z * d);
    else f = exp(-(fog.z * d) * (fog.z * d));
    cur.rgb = lerp(fogcolor.rgb, cur.rgb, saturate(f));
  }
  int func = (int)atest.y;
  if (func != 8 && func != 0) {
    int a8 = (int)round(cur.a * 255);
    int ref = (int)atest.x;
    bool ok = func == 2 ? a8 < ref : func == 3 ? a8 == ref : func == 4 ? a8 <= ref :
                func == 5 ? a8 > ref : func == 6 ? a8 != ref : func == 7 ? a8 >= ref : false;
    if (!ok) discard;
  }
  if (cur.a < acut.x) discard;
  return cur;
}
)";

const char kClearShader[] = R"(
cbuffer P : register(b0) { float4 rect; float4 color; float4 depth; };
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 c = float2(id & 1, id >> 1);
  return float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), depth.x, 1);
}
float4 ps_main() : SV_Target { return color; }
)";

struct VSConstants {
  float wvp[16];
  float vp[4];
  float mode[4];
};

struct PSConstants {
  int32_t scol[2][4];
  int32_t salp[2][4];
  float tfactor[4];
  float fogcolor[4];
  float fog[4];
  float atest[4];
  float key0[4];
  float cut[4];
};

struct ClearConstants {
  float rect[4];
  float color[4];
  float depth[4];
};

struct GpuVertex {
  float pos[4];
  uint32_t diffuse, specular;
  float t0[2], t1[2];
};

struct Pipeline {
  bool ready = false;
  bool failed = false;
  ID3D11VertexShader* vs[2] = {};
  ID3D11PixelShader* ps[2] = {};
  ID3D11InputLayout* layout = nullptr;
  ID3D11VertexShader* clear_vs = nullptr;
  ID3D11PixelShader* clear_ps = nullptr;
  ID3D11Buffer* clear_cb = nullptr;
  ID3D11SamplerState* key_sampler = nullptr;
  ID3D11DepthStencilState* clear_depth[2] = {};  // [writes depth]
  ID3D11BlendState* clear_blend[2] = {};          // [writes colour]
};
Pipeline g_pipe;

bool InitPipeline() {
  if (g_pipe.ready) return true;
  if (g_pipe.failed) return false;
  g_pipe.failed = true;
  Backend& b = B();
  for (int flat = 0; flat < 2; ++flat) {
    std::string src = flat ? std::string("#define FLAT\n") + kShader : std::string(kShader);
    ID3DBlob* vs = Compile(src.c_str(), "vs_main", "vs_4_0");
    ID3DBlob* ps = Compile(src.c_str(), "ps_main", "ps_4_0");
    if (!vs || !ps) {
      SafeRelease(vs);
      SafeRelease(ps);
      return false;
    }
    b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_pipe.vs[flat]);
    b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_pipe.ps[flat]);
    if (!flat) {
      const D3D11_INPUT_ELEMENT_DESC el[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
      };
      b.device->CreateInputLayout(el, 5, vs->GetBufferPointer(), vs->GetBufferSize(), &g_pipe.layout);
    }
    vs->Release();
    ps->Release();
  }
  ID3DBlob* cvs = Compile(kClearShader, "vs_main", "vs_4_0");
  ID3DBlob* cps = Compile(kClearShader, "ps_main", "ps_4_0");
  if (!cvs || !cps) {
    SafeRelease(cvs);
    SafeRelease(cps);
    return false;
  }
  b.device->CreateVertexShader(cvs->GetBufferPointer(), cvs->GetBufferSize(), nullptr, &g_pipe.clear_vs);
  b.device->CreatePixelShader(cps->GetBufferPointer(), cps->GetBufferSize(), nullptr, &g_pipe.clear_ps);
  cvs->Release();
  cps->Release();
  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(ClearConstants);
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  b.device->CreateBuffer(&cb, nullptr, &g_pipe.clear_cb);

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  b.device->CreateSamplerState(&sd, &g_pipe.key_sampler);

  for (int w = 0; w < 2; ++w) {
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = w ? TRUE : FALSE;
    dsd.DepthWriteMask = w ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    b.device->CreateDepthStencilState(&dsd, &g_pipe.clear_depth[w]);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = w ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    b.device->CreateBlendState(&bd, &g_pipe.clear_blend[w]);
  }
  g_pipe.failed = false;
  g_pipe.ready = g_pipe.vs[0] && g_pipe.vs[1] && g_pipe.ps[0] && g_pipe.ps[1] && g_pipe.layout &&
                 g_pipe.clear_vs && g_pipe.clear_ps;
  return g_pipe.ready;
}

void MatMul(const float* a, const float* b, float* out) {
  float r[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                     a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
  std::memcpy(out, r, sizeof(r));
}

D3DMATRIX Identity() {
  D3DMATRIX m{};
  m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
  return m;
}

float AsFloat(DWORD v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

DWORD AsDword(float f) {
  DWORD v;
  std::memcpy(&v, &f, 4);
  return v;
}

void ColorToFloat(DWORD c, float out[4]) {
  out[0] = ((c >> 16) & 0xff) / 255.0f;
  out[1] = ((c >> 8) & 0xff) / 255.0f;
  out[2] = (c & 0xff) / 255.0f;
  out[3] = ((c >> 24) & 0xff) / 255.0f;
}

D3D11_FILTER FilterFor(DWORD mag, DWORD min, DWORD mip) {
  bool mag_lin = mag != 1, min_lin = min != 1, mip_lin = mip == 3;
  if (min == 3 || mag == 4) return D3D11_FILTER_ANISOTROPIC;  // D3DTFN_ANISOTROPIC / D3DTFG_ANISOTROPIC
  int f = (min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0) | (mip_lin ? 0x1 : 0);
  return static_cast<D3D11_FILTER>(f);
}

D3D11_TEXTURE_ADDRESS_MODE AddressFor(DWORD a) {
  switch (a) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
  }
}

D3D11_BLEND BlendFor(DWORD b) {
  if (b >= 1 && b <= 11) return static_cast<D3D11_BLEND>(b);  // same numbering
  return D3D11_BLEND_ONE;
}

}  // namespace

// ------------------------------------------------------------------ FvfLayout

FvfLayout FvfLayout::From(DWORD fvf) {
  FvfLayout l;
  UINT off = 0;
  switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZRHW: off = 16; l.tl = true; break;
    case D3DFVF_XYZ: off = 12; break;
    case D3DFVF_XYZB1: off = 16; break;
    case D3DFVF_XYZB2: off = 20; break;
    case D3DFVF_XYZB3: off = 24; break;
    case D3DFVF_XYZB4: off = 28; break;
    case D3DFVF_XYZB5: off = 32; break;
    default: break;
  }
  if (fvf & D3DFVF_NORMAL) {
    l.normal = static_cast<int>(off);
    off += 12;
  }
  if (fvf & D3DFVF_RESERVED1) off += 4;
  if (fvf & D3DFVF_DIFFUSE) {
    l.diffuse = static_cast<int>(off);
    off += 4;
  }
  if (fvf & D3DFVF_SPECULAR) {
    l.specular = static_cast<int>(off);
    off += 4;
  }
  l.ntex = static_cast<int>((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
  if (l.ntex > 8) l.ntex = 8;
  for (int i = 0; i < l.ntex; ++i) {
    static const int dims[4] = {2, 3, 4, 1};
    l.tex[i] = static_cast<int>(off);
    l.tex_dim[i] = dims[(fvf >> (16 + i * 2)) & 3];
    off += 4 * l.tex_dim[i];
  }
  l.stride = off;
  return l;
}

// --------------------------------------------------------------- VertexBuffer

VertexBuffer::VertexBuffer(const D3DVERTEXBUFFERDESC& desc) : desc_(desc) {
  desc_.dwSize = sizeof(desc_);
  layout_ = FvfLayout::From(desc_.dwFVF);
  data_.assign(static_cast<size_t>(layout_.stride) * desc_.dwNumVertices, 0);
}

HRESULT VertexBuffer::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (riid == IID_IDirect3DVertexBuffer7 || riid == IID_IUnknown_) {
    AddRef();
    *out = this;
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG VertexBuffer::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG VertexBuffer::Release() {
  LONG n = InterlockedDecrement(&refs_);
  if (!n) {
    DD_LOCK;
    delete this;
  }
  return static_cast<ULONG>(n);
}

HRESULT VertexBuffer::Lock(DWORD, void** data, DWORD* size) {
  if (!data) return DDERR_INVALIDPARAMS;
  *data = data_.data();
  if (size) *size = static_cast<DWORD>(data_.size());
  return D3D_OK;
}

HRESULT VertexBuffer::Unlock() { return D3D_OK; }

bool VertexBuffer::HasClipData(DWORD first, DWORD n) const {
  if (!layout_.tl || static_cast<size_t>(first) + n > processed_valid_.size()) return false;
  for (DWORD i = first; i < first + n; ++i) {
    if (!processed_valid_[i]) return false;
    if (std::memcmp(data_.data() + static_cast<size_t>(i) * layout_.stride, processed_[i].screen, 16) != 0)
      return false;
  }
  return true;
}

HRESULT VertexBuffer::ProcessVertices(DWORD op, DWORD dst_index, DWORD count,
                                      IDirect3DVertexBuffer7* src_in, DWORD src_index,
                                      IDirect3DDevice7* dev_in, DWORD flags) {
  DD_LOCK;
  auto* src = static_cast<VertexBuffer*>(src_in);
  auto* dev = static_cast<Device*>(dev_in);
  if (!src || !dev) return DDERR_INVALIDPARAMS;
  if (static_cast<size_t>(dst_index) + count > desc_.dwNumVertices ||
      static_cast<size_t>(src_index) + count > src->desc_.dwNumVertices)
    return DDERR_INVALIDPARAMS;
  const FvfLayout& sl = src->layout_;
  const FvfLayout& dl = layout_;
  if (!dl.tl) {
    LogOnce("ProcessVertices into a non-XYZRHW buffer (fvf %lx) unsupported", desc_.dwFVF);
    return D3D_OK;
  }
  if (op & D3DVOP_LIGHT) LogOnce("ProcessVertices: lighting requested, not emulated");
  const float* m = dev->Wvp();
  SmoothNoteDraw("process", sl.tl, count);
  const D3DVIEWPORT7& vp = dev->Viewport();
  processed_vp_ = vp;
  if (processed_.size() < desc_.dwNumVertices) {
    processed_.resize(desc_.dwNumVertices);
    processed_valid_.resize(desc_.dwNumVertices, 0);
  }
  bool copy = !(flags & D3DPV_DONOTCOPYDATA);
  float hw = vp.dwWidth * 0.5f, hh = vp.dwHeight * 0.5f, dz = vp.dvMaxZ - vp.dvMinZ;
  for (DWORD i = 0; i < count; ++i) {
    const uint8_t* s = src->data_.data() + static_cast<size_t>(src_index + i) * sl.stride;
    uint8_t* d = data_.data() + static_cast<size_t>(dst_index + i) * dl.stride;
    float p[3];
    std::memcpy(p, s, 12);
    Processed& out = processed_[dst_index + i];
    float screen[4];
    if (sl.tl) {
      std::memcpy(screen, s, 16);
      processed_valid_[dst_index + i] = 0;
    } else {
      float c[4];
      for (int k = 0; k < 4; ++k) c[k] = p[0] * m[0 * 4 + k] + p[1] * m[1 * 4 + k] + p[2] * m[2 * 4 + k] + m[3 * 4 + k];
      float w = c[3];
      float rhw = w != 0 ? 1.0f / w : 0.0f;
      screen[0] = vp.dwX + (1.0f + c[0] * rhw) * hw;
      screen[1] = vp.dwY + (1.0f - c[1] * rhw) * hh;
      screen[2] = vp.dvMinZ + c[2] * rhw * dz;
      screen[3] = rhw;
      std::memcpy(out.clip, c, 16);
      std::memcpy(out.screen, screen, 16);
      processed_valid_[dst_index + i] = 1;
    }
    std::memcpy(d, screen, 16);
    if (copy) {
      if (dl.diffuse >= 0) {
        DWORD v = 0xffffffff;
        if (sl.diffuse >= 0) std::memcpy(&v, s + sl.diffuse, 4);
        std::memcpy(d + dl.diffuse, &v, 4);
      }
      if (dl.specular >= 0) {
        DWORD v = 0;
        if (sl.specular >= 0) std::memcpy(&v, s + sl.specular, 4);
        std::memcpy(d + dl.specular, &v, 4);
      }
      for (int t = 0; t < dl.ntex && t < sl.ntex; ++t)
        std::memcpy(d + dl.tex[t], s + sl.tex[t], 4 * std::min(dl.tex_dim[t], sl.tex_dim[t]));
    }
  }
  return D3D_OK;
}

HRESULT VertexBuffer::GetVertexBufferDesc(D3DVERTEXBUFFERDESC* desc) {
  if (!desc) return DDERR_INVALIDPARAMS;
  *desc = desc_;
  return D3D_OK;
}

HRESULT VertexBuffer::Optimize(IDirect3DDevice7*, DWORD) { return D3D_OK; }

HRESULT VertexBuffer::ProcessVerticesStrided(DWORD, DWORD, DWORD, D3DDRAWPRIMITIVESTRIDEDDATA*,
                                             DWORD, IDirect3DDevice7*, DWORD) {
  LogOnce("ProcessVerticesStrided unsupported");
  return DDERR_UNSUPPORTED;
}

// --------------------------------------------------------------------- Device

Device::Device(DirectDraw* dd, Surface* rt, REFCLSID cls) : dd_(dd), rt_(rt), guid_(cls) {
  dd_->AddRef();
  rt_->AddRef();
  rt_->SetUsedAsRenderTarget();
  world_ = view_ = proj_ = world_set_ = view_set_ = Identity();
  viewport_ = {0, 0, rt->Width(), rt->Height(), 0.0f, 1.0f};
  rs_[RS_TEXTUREPERSPECTIVE] = 1;
  rs_[RS_ZENABLE] = rt->ZBuffer() ? 1 : 0;
  rs_[RS_FILLMODE] = 3;
  rs_[RS_SHADEMODE] = 2;
  rs_[RS_ZWRITEENABLE] = 1;
  rs_[RS_LASTPIXEL] = 1;
  rs_[RS_SRCBLEND] = 2;
  rs_[RS_DESTBLEND] = 1;
  rs_[RS_CULLMODE] = D3DCULL_CCW;
  rs_[RS_ZFUNC] = 4;
  rs_[RS_ALPHAFUNC] = 8;
  rs_[RS_FOGEND] = AsDword(1.0f);
  rs_[RS_FOGDENSITY] = AsDword(1.0f);
  rs_[RS_TEXTUREFACTOR] = 0xffffffff;
  rs_[RS_CLIPPING] = 1;
  rs_[RS_LIGHTING] = 1;
  rs_[RS_COLORVERTEX] = 1;
  rs_[RS_DIFFUSEMATERIALSOURCE] = 1;
  rs_[RS_SPECULARMATERIALSOURCE] = 2;
  for (int s = 0; s < 8; ++s) {
    tss_[s][TSS_COLOROP] = s == 0 ? 4 : 1;
    tss_[s][TSS_COLORARG1] = 2;
    tss_[s][TSS_COLORARG2] = 1;
    tss_[s][TSS_ALPHAOP] = s == 0 ? 2 : 1;
    tss_[s][TSS_ALPHAARG1] = 2;
    tss_[s][TSS_ALPHAARG2] = 1;
    tss_[s][TSS_TEXCOORDINDEX] = static_cast<DWORD>(s);
    tss_[s][TSS_ADDRESS] = tss_[s][TSS_ADDRESSU] = tss_[s][TSS_ADDRESSV] = 1;
    tss_[s][TSS_MAGFILTER] = 1;
    tss_[s][TSS_MINFILTER] = 1;
    tss_[s][TSS_MIPFILTER] = 1;
  }
}

Device::~Device() {
  for (Surface*& t : tex_) SafeRelease(t);
  SafeRelease(rt_);
  for (auto& kv : blend_cache_) kv.second->Release();
  for (auto& kv : depth_cache_) kv.second->Release();
  for (auto& kv : raster_cache_) kv.second->Release();
  for (auto& kv : sampler_cache_) kv.second->Release();
  SafeRelease(vbuf_);
  SafeRelease(ibuf_);
  SafeRelease(vs_cb_);
  SafeRelease(ps_cb_);
  dd_->Release();
}

bool Device::Init() {
  if (!InitPipeline()) return false;
  Backend& b = B();
  D3D11_BUFFER_DESC bd{};
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.ByteWidth = vbuf_size_ = 8u << 20;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  b.device->CreateBuffer(&bd, nullptr, &vbuf_);
  bd.ByteWidth = ibuf_size_ = 2u << 20;
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  b.device->CreateBuffer(&bd, nullptr, &ibuf_);
  D3D11_BUFFER_DESC cb{};
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.ByteWidth = (sizeof(VSConstants) + 15) & ~15u;
  b.device->CreateBuffer(&cb, nullptr, &vs_cb_);
  cb.ByteWidth = (sizeof(PSConstants) + 15) & ~15u;
  b.device->CreateBuffer(&cb, nullptr, &ps_cb_);
  return vbuf_ && ibuf_ && vs_cb_ && ps_cb_;
}

void Device::FillCaps(D3DDEVICEDESC7& d, const GUID& guid) {
  std::memset(&d, 0, sizeof(d));
  bool hw = guid != IID_IDirect3DRGBDevice;
  d.dwDevCaps = 0x1 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x8000 |
                (hw ? 0x80000 : 0) | (guid == IID_IDirect3DTnLHalDevice ? 0x10000 : 0);
  D3DPRIMCAPS p{};
  p.dwSize = sizeof(p);
  p.dwMiscCaps = 0x2 | 0x10 | 0x20 | 0x40;
  p.dwRasterCaps = 0x1 | 0x10 | 0x20 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x20000 |
                   0x100000 | 0x200000;
  p.dwZCmpCaps = 0xff;
  p.dwSrcBlendCaps = 0x1fff;
  p.dwDestBlendCaps = 0x1fff;
  p.dwAlphaCmpCaps = 0xff;
  p.dwShadeCaps = 0x2 | 0x8 | 0x20 | 0x200 | 0x1000 | 0x4000 | 0x40000 | 0x80000;
  p.dwTextureCaps = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;
  p.dwTextureFilterCaps = 0x0B03073F;
  p.dwTextureBlendCaps = 0xff;
  p.dwTextureAddressCaps = 0x1f;
  d.dpcLineCaps = p;
  d.dpcTriCaps = p;
  d.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
  d.dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
  d.dwMinTextureWidth = d.dwMinTextureHeight = 1;
  d.dwMaxTextureWidth = d.dwMaxTextureHeight = 4096;
  d.dwMaxTextureRepeat = 8192;
  d.dwMaxTextureAspectRatio = 4096;
  d.dwMaxAnisotropy = 16;
  d.dwStencilCaps = 0xff;
  d.dwFVFCaps = 8;
  d.dwTextureOpCaps = 0xffff;
  d.wMaxTextureBlendStages = kStages;
  d.wMaxSimultaneousTextures = kStages;
  d.dwMaxActiveLights = 8;
  d.dvMaxVertexW = 1e10f;
  d.deviceGUID = guid;
}

HRESULT Device::QueryInterface(REFIID riid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (riid == IID_IDirect3DDevice7 || riid == IID_IUnknown_) {
    AddRef();
    *out = this;
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG Device::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG Device::Release() {
  LONG n = InterlockedDecrement(&refs_);
  if (!n) {
    DD_LOCK;
    delete this;
  }
  return static_cast<ULONG>(n);
}

HRESULT Device::GetCaps(D3DDEVICEDESC7* desc) {
  if (!desc) return DDERR_INVALIDPARAMS;
  FillCaps(*desc, guid_);
  return D3D_OK;
}

HRESULT Device::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) {
  if (!cb) return DDERR_INVALIDPARAMS;
  DDPIXELFORMAT formats[] = {
      MakeRGBFormat(16, 0xf800, 0x07e0, 0x001f, 0),
      MakeRGBFormat(16, 0x7c00, 0x03e0, 0x001f, 0),
      MakeRGBFormat(16, 0x7c00, 0x03e0, 0x001f, 0x8000),
      MakeRGBFormat(16, 0x0f00, 0x00f0, 0x000f, 0xf000),
  };
  for (DDPIXELFORMAT& f : formats)
    if (cb(&f, ctx) == D3DENUMRET_CANCEL) break;
  return D3D_OK;
}

HRESULT Device::BeginScene() { return D3D_OK; }
HRESULT Device::EndScene() { return D3D_OK; }

HRESULT Device::GetDirect3D(IDirect3D7** out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = dd_->D3D();
  (*out)->AddRef();
  return D3D_OK;
}

HRESULT Device::SetRenderTarget(IDirectDrawSurface7* s, DWORD) {
  DD_LOCK;
  Surface* t = Surface::From(s);
  if (!t) return DDERR_INVALIDPARAMS;
  t->AddRef();
  t->SetUsedAsRenderTarget();
  SafeRelease(rt_);
  rt_ = t;
  LogOnce("SetRenderTarget %ux%u", t->Width(), t->Height());
  return D3D_OK;
}

HRESULT Device::GetRenderTarget(IDirectDrawSurface7** s) {
  if (!s) return DDERR_INVALIDPARAMS;
  *s = rt_;
  rt_->AddRef();
  return D3D_OK;
}

HRESULT Device::Clear(DWORD count, D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD) {
  DD_LOCK;
  if (!rt_ || !Ready() || !g_pipe.ready) return D3D_OK;
  RECT vp{static_cast<LONG>(viewport_.dwX), static_cast<LONG>(viewport_.dwY),
          static_cast<LONG>(viewport_.dwX + viewport_.dwWidth),
          static_cast<LONG>(viewport_.dwY + viewport_.dwHeight)};
  const bool whole = (!count || !rects) && vp.left <= 0 && vp.top <= 0 &&
                     vp.right >= static_cast<LONG>(rt_->Width()) &&
                     vp.bottom >= static_cast<LONG>(rt_->Height());
  bool has_depth = false;
  if (!BindTarget(has_depth, whole && (flags & D3DCLEAR_TARGET))) return D3D_OK;
  Backend& b = B();
  GpuTexture* rt = rt_->GpuForDraw();
  bool clear_color = (flags & D3DCLEAR_TARGET) != 0;
  bool clear_depth = (flags & D3DCLEAR_ZBUFFER) && has_depth && cur_dsv_;
  if (!clear_color && !clear_depth) return D3D_OK;
  std::vector<RECT> list;
  if (!count || !rects) {
    list.push_back(vp);
  } else {
    for (DWORD i = 0; i < count; ++i) {
      RECT r{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2}, o;
      if (IntersectRect(&o, &r, &vp)) list.push_back(o);
    }
  }
  float c[4];
  ColorToFloat(color, c);
  float rgba[4] = {c[0], c[1], c[2], c[3]};
  for (const RECT& r : list) {
    bool full = r.left <= 0 && r.top <= 0 && r.right >= static_cast<LONG>(rt->width) &&
                r.bottom >= static_cast<LONG>(rt->height);
    if (full) {
      if (clear_color) b.ctx->ClearRenderTargetView(cur_rtv_, rgba);
      if (clear_depth) b.ctx->ClearDepthStencilView(cur_dsv_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, z, 0);
      continue;
    }
    ClearConstants cc{};
    cc.rect[0] = 2.0f * r.left / rt->width - 1.0f;
    cc.rect[1] = 1.0f - 2.0f * r.top / rt->height;
    cc.rect[2] = 2.0f * r.right / rt->width - 1.0f;
    cc.rect[3] = 1.0f - 2.0f * r.bottom / rt->height;
    std::memcpy(cc.color, rgba, sizeof(rgba));
    cc.depth[0] = z;
    b.ctx->UpdateSubresource(g_pipe.clear_cb, 0, nullptr, &cc, 0, 0);
    b.ctx->OMSetRenderTargets(1, &cur_rtv_, clear_depth ? cur_dsv_ : nullptr);
    D3D11_VIEWPORT v{0, 0, static_cast<float>(rt->width), static_cast<float>(rt->height), 0, 1};
    b.ctx->RSSetViewports(1, &v);
    b.ctx->RSSetState(b.blit_raster);
    b.ctx->OMSetDepthStencilState(g_pipe.clear_depth[clear_depth ? 1 : 0], 0);
    b.ctx->OMSetBlendState(g_pipe.clear_blend[clear_color ? 1 : 0], nullptr, 0xffffffff);
    b.ctx->IASetInputLayout(nullptr);
    b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    b.ctx->VSSetShader(g_pipe.clear_vs, nullptr, 0);
    b.ctx->VSSetConstantBuffers(0, 1, &g_pipe.clear_cb);
    b.ctx->PSSetShader(g_pipe.clear_ps, nullptr, 0);
    b.ctx->Draw(4, 0);
  }
  if (clear_color) {
    if (cur_msaa_) MsaaMarkDrawn();
    rt_->MarkGpuWritten();
  }
  return D3D_OK;
}

HRESULT Device::SetTransform(DWORD type, D3DMATRIX* m) {
  if (!m) return DDERR_INVALIDPARAMS;
  switch (type) {
    case D3DTRANSFORMSTATE_WORLD:
      world_set_ = *m;
      SmoothTransform(type, &m->m[0][0], m, &world_.m[0][0]);
      break;
    case D3DTRANSFORMSTATE_VIEW:
      view_set_ = *m;
      SmoothTransform(type, &m->m[0][0], m, &view_.m[0][0]);
      break;
    case D3DTRANSFORMSTATE_PROJECTION: {
      proj_set_ = *m;
      proj_ = *m;
      const float r = SmoothProjectionScale(&m->m[0][0]);  // undo the smoothing cull margin
      proj_.m[0][0] *= r;
      proj_.m[1][1] *= r;
      if (m->m[2][3] != 0.0f && m->m[2][2] != 1.0f && m->m[2][2] != 0.0f) {
        const float zn = -m->m[3][2] / m->m[2][2], zf = m->m[3][2] / (1.0f - m->m[2][2]);
        LogOnce("projection near %.2f far %.0f", zn, zf);
      }
      break;
    }
    default: LogOnce("SetTransform type %lu ignored", type); return D3D_OK;
  }
  wvp_dirty_ = true;
  return D3D_OK;
}

HRESULT Device::GetTransform(DWORD type, D3DMATRIX* m) {
  if (!m) return DDERR_INVALIDPARAMS;
  switch (type) {
    case D3DTRANSFORMSTATE_WORLD: *m = world_set_; break;  // what the game set
    case D3DTRANSFORMSTATE_VIEW: *m = view_set_; break;
    case D3DTRANSFORMSTATE_PROJECTION: *m = proj_set_; break;
    default: *m = Identity(); break;
  }
  return D3D_OK;
}

HRESULT Device::MultiplyTransform(DWORD type, D3DMATRIX* m) {
  if (!m) return DDERR_INVALIDPARAMS;
  D3DMATRIX cur;
  GetTransform(type, &cur);
  D3DMATRIX r;
  MatMul(&m->m[0][0], &cur.m[0][0], &r.m[0][0]);
  return SetTransform(type, &r);
}

const float* Device::Wvp() {
  if (wvp_dirty_) {
    float wv[16];
    MatMul(&world_.m[0][0], &view_.m[0][0], wv);
    MatMul(wv, &proj_.m[0][0], wvp_);
    wvp_dirty_ = false;
  }
  return wvp_;
}

HRESULT Device::SetViewport(D3DVIEWPORT7* vp) {
  if (!vp) return DDERR_INVALIDPARAMS;
  if (vp->dwWidth != viewport_.dwWidth || vp->dwHeight != viewport_.dwHeight) wvp_dirty_ = true;
  viewport_ = *vp;
  return D3D_OK;
}

HRESULT Device::GetViewport(D3DVIEWPORT7* vp) {
  if (!vp) return DDERR_INVALIDPARAMS;
  *vp = viewport_;
  return D3D_OK;
}

HRESULT Device::SetMaterial(D3DMATERIAL7* m) {
  if (!m) return DDERR_INVALIDPARAMS;
  material_ = *m;
  return D3D_OK;
}

HRESULT Device::GetMaterial(D3DMATERIAL7* m) {
  if (!m) return DDERR_INVALIDPARAMS;
  *m = material_;
  return D3D_OK;
}

HRESULT Device::SetLight(DWORD i, D3DLIGHT7* l) {
  if (!l) return DDERR_INVALIDPARAMS;
  lights_[i] = *l;
  return D3D_OK;
}

HRESULT Device::GetLight(DWORD i, D3DLIGHT7* l) {
  if (!l) return DDERR_INVALIDPARAMS;
  auto it = lights_.find(i);
  if (it == lights_.end()) return DDERR_INVALIDPARAMS;
  *l = it->second;
  return D3D_OK;
}

HRESULT Device::SetRenderState(DWORD state, DWORD value) {
  if (state >= 256) return DDERR_INVALIDPARAMS;
  rs_[state] = value;
  return D3D_OK;
}

HRESULT Device::GetRenderState(DWORD state, DWORD* value) {
  if (state >= 256 || !value) return DDERR_INVALIDPARAMS;
  *value = rs_[state];
  return D3D_OK;
}

HRESULT Device::BeginStateBlock() {
  LogOnce("state blocks unsupported");
  return DDERR_UNSUPPORTED;
}
HRESULT Device::EndStateBlock(DWORD*) { return DDERR_UNSUPPORTED; }
HRESULT Device::ApplyStateBlock(DWORD) { return DDERR_UNSUPPORTED; }
HRESULT Device::CaptureStateBlock(DWORD) { return DDERR_UNSUPPORTED; }
HRESULT Device::DeleteStateBlock(DWORD) { return DDERR_UNSUPPORTED; }
HRESULT Device::CreateStateBlock(DWORD, DWORD*) {
  LogOnce("state blocks unsupported");
  return DDERR_UNSUPPORTED;
}

HRESULT Device::PreLoad(IDirectDrawSurface7* s) {
  DD_LOCK;
  if (Surface* t = Surface::From(s)) t->Gpu();
  return D3D_OK;
}

HRESULT Device::DrawPrimitive(DWORD prim, DWORD fvf, void* v, DWORD n, DWORD) {
  DD_LOCK;
  if (!v || !n) return D3D_OK;
  Draw d;
  d.prim = prim;
  d.verts = static_cast<const uint8_t*>(v);
  d.layout = FvfLayout::From(fvf);
  d.count = n;
  SmoothNoteDraw("draw", d.layout.tl, n);
  if (!d.layout.tl) d.verts = SmoothVertices(d.verts, d.layout.stride, n, nullptr);
  Submit(d);
  return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitive(DWORD prim, DWORD fvf, void* v, DWORD nv, WORD* idx,
                                     DWORD ni, DWORD) {
  DD_LOCK;
  if (!v || !nv || !idx || !ni) return D3D_OK;
  Draw d;
  d.prim = prim;
  d.verts = static_cast<const uint8_t*>(v);
  d.layout = FvfLayout::From(fvf);
  d.count = nv;
  d.idx = idx;
  d.nidx = ni;
  SmoothNoteDraw("drawidx", d.layout.tl, d.count);
  if (!d.layout.tl) d.verts = SmoothVertices(d.verts, d.layout.stride, nv, idx);
  Submit(d);
  return D3D_OK;
}

HRESULT Device::SetClipStatus(D3DCLIPSTATUS* s) {
  if (s) clip_status_ = *s;
  return D3D_OK;
}

HRESULT Device::GetClipStatus(D3DCLIPSTATUS* s) {
  if (!s) return DDERR_INVALIDPARAMS;
  *s = clip_status_;
  return D3D_OK;
}

namespace {
// Interleave strided vertex data into the FVF's normal layout.
void Interleave(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& sd, DWORD n, std::vector<uint8_t>& out,
                FvfLayout& l) {
  l = FvfLayout::From(fvf);
  out.assign(static_cast<size_t>(l.stride) * n, 0);
  UINT pos_bytes = l.tl ? 16 : 12;
  for (DWORD i = 0; i < n; ++i) {
    uint8_t* d = out.data() + static_cast<size_t>(i) * l.stride;
    auto at = [i](const D3DDP_PTRSTRIDE& p) {
      return static_cast<const uint8_t*>(p.lpvData) + static_cast<size_t>(i) * p.dwStride;
    };
    if (sd.position.lpvData) std::memcpy(d, at(sd.position), pos_bytes);
    if (l.normal >= 0 && sd.normal.lpvData) std::memcpy(d + l.normal, at(sd.normal), 12);
    if (l.diffuse >= 0 && sd.diffuse.lpvData) std::memcpy(d + l.diffuse, at(sd.diffuse), 4);
    if (l.specular >= 0 && sd.specular.lpvData) std::memcpy(d + l.specular, at(sd.specular), 4);
    for (int t = 0; t < l.ntex; ++t)
      if (sd.textureCoords[t].lpvData) std::memcpy(d + l.tex[t], at(sd.textureCoords[t]), 4 * l.tex_dim[t]);
  }
}
}  // namespace

HRESULT Device::DrawPrimitiveStrided(DWORD prim, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* sd, DWORD n,
                                     DWORD) {
  DD_LOCK;
  if (!sd || !n) return D3D_OK;
  std::vector<uint8_t> buf;
  Draw d;
  Interleave(fvf, *sd, n, buf, d.layout);
  d.prim = prim;
  d.verts = buf.data();
  d.count = n;
  SmoothNoteDraw("draw", d.layout.tl, n);
  Submit(d);
  return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitiveStrided(DWORD prim, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* sd,
                                            DWORD nv, WORD* idx, DWORD ni, DWORD) {
  DD_LOCK;
  if (!sd || !nv || !idx || !ni) return D3D_OK;
  std::vector<uint8_t> buf;
  Draw d;
  Interleave(fvf, *sd, nv, buf, d.layout);
  d.prim = prim;
  d.verts = buf.data();
  d.count = nv;
  d.idx = idx;
  d.nidx = ni;
  SmoothNoteDraw("drawidx", d.layout.tl, d.count);
  if (!d.layout.tl) d.verts = SmoothVertices(d.verts, d.layout.stride, nv, idx);
  Submit(d);
  return D3D_OK;
}

HRESULT Device::DrawPrimitiveVB(DWORD prim, IDirect3DVertexBuffer7* vb_in, DWORD start, DWORD n,
                                DWORD) {
  DD_LOCK;
  auto* vb = static_cast<VertexBuffer*>(vb_in);
  if (!vb || !n) return D3D_OK;
  if (static_cast<size_t>(start) + n > vb->Count()) return DDERR_INVALIDPARAMS;
  Draw d;
  d.prim = prim;
  d.verts = vb->Data();
  d.layout = vb->Layout();
  d.first = start;
  d.count = n;
  d.vb = vb;
  SmoothNoteDraw("drawvb", d.layout.tl, d.count);
  Submit(d);
  return D3D_OK;
}

HRESULT Device::DrawIndexedPrimitiveVB(DWORD prim, IDirect3DVertexBuffer7* vb_in, DWORD start,
                                       DWORD nv, WORD* idx, DWORD ni, DWORD) {
  DD_LOCK;
  auto* vb = static_cast<VertexBuffer*>(vb_in);
  if (!vb || !nv || !idx || !ni) return D3D_OK;
  if (static_cast<size_t>(start) + nv > vb->Count()) nv = vb->Count() - start;
  Draw d;
  d.prim = prim;
  d.verts = vb->Data();
  d.layout = vb->Layout();
  d.first = start;
  d.count = nv;
  d.idx = idx;
  d.nidx = ni;
  d.vb = vb;
  SmoothNoteDraw("drawvb", d.layout.tl, d.count);
  Submit(d);
  return D3D_OK;
}

HRESULT Device::ComputeSphereVisibility(D3DVECTOR*, float*, DWORD n, DWORD, DWORD* out) {
  LogOnce("ComputeSphereVisibility approximated (all visible)");
  if (out)
    for (DWORD i = 0; i < n; ++i) out[i] = 0;
  return D3D_OK;
}

HRESULT Device::GetTexture(DWORD stage, IDirectDrawSurface7** t) {
  if (!t || stage >= 8) return DDERR_INVALIDPARAMS;
  *t = tex_[stage];
  if (tex_[stage]) tex_[stage]->AddRef();
  return D3D_OK;
}

HRESULT Device::SetTexture(DWORD stage, IDirectDrawSurface7* t) {
  if (stage >= 8) return DDERR_INVALIDPARAMS;
  Surface* s = Surface::From(t);
  if (t && !s) return DDERR_INVALIDPARAMS;
  if (s == tex_[stage]) return D3D_OK;
  DD_LOCK;
  if (s) s->AddRef();
  SafeRelease(tex_[stage]);
  tex_[stage] = s;
  return D3D_OK;
}

HRESULT Device::GetTextureStageState(DWORD stage, DWORD type, DWORD* value) {
  if (stage >= 8 || type >= 32 || !value) return DDERR_INVALIDPARAMS;
  *value = tss_[stage][type];
  return D3D_OK;
}

HRESULT Device::SetTextureStageState(DWORD stage, DWORD type, DWORD value) {
  if (stage >= 8 || type >= 32) return DDERR_INVALIDPARAMS;
  tss_[stage][type] = value;
  if (type == TSS_ADDRESS) tss_[stage][TSS_ADDRESSU] = tss_[stage][TSS_ADDRESSV] = value;
  return D3D_OK;
}

HRESULT Device::ValidateDevice(DWORD* passes) {
  if (passes) *passes = 1;
  return D3D_OK;
}

HRESULT Device::Load(IDirectDrawSurface7* dst, POINT* dp, IDirectDrawSurface7* src, RECT* sr, DWORD) {
  DD_LOCK;
  Surface* d = Surface::From(dst);
  Surface* s = Surface::From(src);
  if (!d || !s) return DDERR_INVALIDPARAMS;
  d->CopyFrom(s, dp, sr);
  return D3D_OK;
}

HRESULT Device::LightEnable(DWORD i, BOOL on) {
  light_on_[i] = on;
  return D3D_OK;
}

HRESULT Device::GetLightEnable(DWORD i, BOOL* on) {
  if (!on) return DDERR_INVALIDPARAMS;
  auto it = light_on_.find(i);
  *on = it != light_on_.end() && it->second;
  return D3D_OK;
}

HRESULT Device::SetClipPlane(DWORD, float*) {
  LogOnce("user clip planes unsupported");
  return D3D_OK;
}

HRESULT Device::GetClipPlane(DWORD, float* p) {
  if (p) std::memset(p, 0, 16);
  return D3D_OK;
}

HRESULT Device::GetInfo(DWORD, void*, DWORD) { return S_FALSE; }

// ------------------------------------------------------------------- drawing

UINT Device::PushVertices(const void* data, UINT bytes) {
  Backend& b = B();
  D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (vbuf_pos_ + bytes > vbuf_size_) {
    vbuf_pos_ = 0;
    map = D3D11_MAP_WRITE_DISCARD;
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(b.ctx->Map(vbuf_, 0, map, 0, &m))) return UINT(-1);
  std::memcpy(static_cast<uint8_t*>(m.pData) + vbuf_pos_, data, bytes);
  b.ctx->Unmap(vbuf_, 0);
  UINT at = vbuf_pos_;
  vbuf_pos_ += bytes;
  return at;
}

UINT Device::PushIndices(const void* data, UINT bytes) {
  Backend& b = B();
  D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
  UINT need = (bytes + 3) & ~3u;
  if (ibuf_pos_ + need > ibuf_size_) {
    ibuf_pos_ = 0;
    map = D3D11_MAP_WRITE_DISCARD;
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(b.ctx->Map(ibuf_, 0, map, 0, &m))) return UINT(-1);
  std::memcpy(static_cast<uint8_t*>(m.pData) + ibuf_pos_, data, bytes);
  b.ctx->Unmap(ibuf_, 0);
  UINT at = ibuf_pos_;
  ibuf_pos_ += need;
  return at;
}

bool Device::BindTarget(bool& has_depth, bool full_clear) {
  if (!rt_ || !Ready() || !g_pipe.ready) return false;
  GpuTexture* rt = rt_->GpuForDraw();
  if (!rt->rtv) return false;
  ID3D11DepthStencilView* dsv = nullptr;
  if (Surface* z = rt_->ZBuffer()) {
    GpuTexture* zt = z->Gpu();
    if (zt->dsv && zt->width == rt->width && zt->height == rt->height) dsv = zt->dsv;
  }
  has_depth = dsv != nullptr;
  // Never sample from the texture being drawn into.
  ID3D11ShaderResourceView* none[2] = {};
  B().ctx->PSSetShaderResources(0, 2, none);
  // 3D scenes (targets with a depth buffer) are drawn multisampled.
  MsaaViews mv;
  cur_msaa_ = has_depth && MsaaAcquire(rt, mv, full_clear);
  if (cur_msaa_) {
    cur_rtv_ = mv.rtv;
    cur_dsv_ = mv.dsv;
  } else {
    MsaaRelease(rt->tex);
    cur_rtv_ = rt->rtv;
    cur_dsv_ = dsv;
  }
  B().ctx->OMSetRenderTargets(1, &cur_rtv_, cur_dsv_);
  return true;
}

ID3D11BlendState* Device::GetBlend() {
  bool on = rs_[RS_ALPHABLENDENABLE] != 0;
  DWORD src = rs_[RS_SRCBLEND], dst = rs_[RS_DESTBLEND];
  if (src == 12) {  // D3DBLEND_BOTHSRCALPHA
    src = 5;
    dst = 6;
  } else if (src == 13) {  // D3DBLEND_BOTHINVSRCALPHA
    src = 6;
    dst = 5;
  }
  uint32_t key = on ? (1u | (src << 1) | (dst << 8)) : 0;
  auto it = blend_cache_.find(key);
  if (it != blend_cache_.end()) return it->second;
  D3D11_BLEND_DESC bd{};
  auto& t = bd.RenderTarget[0];
  t.BlendEnable = on;
  t.SrcBlend = BlendFor(src);
  t.DestBlend = BlendFor(dst);
  t.BlendOp = D3D11_BLEND_OP_ADD;
  // Alpha factors cannot use colour terms.
  auto alpha_of = [](D3D11_BLEND b) {
    switch (b) {
      case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
      case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
      case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
      case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
      default: return b;
    }
  };
  t.SrcBlendAlpha = alpha_of(t.SrcBlend);
  t.DestBlendAlpha = alpha_of(t.DestBlend);
  t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState* s = nullptr;
  B().device->CreateBlendState(&bd, &s);
  blend_cache_[key] = s;
  return s;
}

ID3D11DepthStencilState* Device::GetDepth(bool has_depth) {
  bool on = has_depth && rs_[RS_ZENABLE] != 0;
  bool write = on && rs_[RS_ZWRITEENABLE] != 0;
  DWORD func = rs_[RS_ZFUNC];
  if (func < 1 || func > 8) func = 4;
  uint32_t key = on ? (1u | (write ? 2u : 0u) | (func << 2)) : 0;
  auto it = depth_cache_.find(key);
  if (it != depth_cache_.end()) return it->second;
  D3D11_DEPTH_STENCIL_DESC d{};
  d.DepthEnable = on;
  d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  d.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(func);  // same numbering
  ID3D11DepthStencilState* s = nullptr;
  B().device->CreateDepthStencilState(&d, &s);
  depth_cache_[key] = s;
  return s;
}

ID3D11RasterizerState* Device::GetRaster(bool depth_clip) {
  DWORD cull = rs_[RS_CULLMODE], fill = rs_[RS_FILLMODE], bias = rs_[RS_ZBIAS] & 0xff;
  uint32_t key = (cull & 3) | ((fill & 3) << 2) | (depth_clip ? 16u : 0u) | (bias << 8);
  auto it = raster_cache_.find(key);
  if (it != raster_cache_.end()) return it->second;
  D3D11_RASTERIZER_DESC r{};
  r.FillMode = fill == 3 ? D3D11_FILL_SOLID : D3D11_FILL_WIREFRAME;
  r.CullMode = cull == D3DCULL_CW ? D3D11_CULL_FRONT : cull == D3DCULL_CCW ? D3D11_CULL_BACK : D3D11_CULL_NONE;
  r.FrontCounterClockwise = FALSE;
  r.DepthBias = -static_cast<INT>(bias) * 16;
  r.SlopeScaledDepthBias = -static_cast<float>(bias) * 0.5f;
  r.DepthClipEnable = depth_clip;
  ID3D11RasterizerState* s = nullptr;
  B().device->CreateRasterizerState(&r, &s);
  raster_cache_[key] = s;
  return s;
}

ID3D11SamplerState* Device::GetSampler(int stage) {
  const DWORD* t = tss_[stage];
  DWORD mag = t[TSS_MAGFILTER] & 7, min = t[TSS_MINFILTER] & 7, mip = t[TSS_MIPFILTER] & 3;
  DWORD au = t[TSS_ADDRESSU] & 7, av = t[TSS_ADDRESSV] & 7;
  int bias = static_cast<int>(AsFloat(t[TSS_MIPMAPLODBIAS]) * 4.0f) & 0xff;
  DWORD maxlevel = t[TSS_MAXMIPLEVEL] & 0xf;
  uint32_t key = mag | (min << 3) | (mip << 6) | (au << 8) | (av << 11) | (maxlevel << 14) |
                 (static_cast<uint32_t>(bias) << 18);
  auto it = sampler_cache_.find(key);
  if (it != sampler_cache_.end()) return it->second;
  D3D11_SAMPLER_DESC s{};
  s.Filter = FilterFor(mag, min, mip);
  s.AddressU = AddressFor(au);
  s.AddressV = AddressFor(av);
  s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  s.MaxAnisotropy = s.Filter == D3D11_FILTER_ANISOTROPIC ? 16 : 1;
  // Anisotropic filtering (ini Anisotropy) for every texture the game
  // filters smoothly; point-sampled ones (text, pixel art) stay as they are.
  const Config& cfg = GetConfig();
  if (cfg.anisotropy > 1 && min != 1) {
    s.Filter = D3D11_FILTER_ANISOTROPIC;
    s.MaxAnisotropy = static_cast<UINT>(cfg.anisotropy);
  }
  s.MipLODBias = AsFloat(t[TSS_MIPMAPLODBIAS]) + cfg.mip_bias;
  s.MinLOD = static_cast<float>(maxlevel);
  s.MaxLOD = mip == 1 ? static_cast<float>(maxlevel) : D3D11_FLOAT32_MAX;
  s.ComparisonFunc = D3D11_COMPARISON_NEVER;
  ColorToFloat(t[TSS_BORDERCOLOR], s.BorderColor);
  ID3D11SamplerState* out = nullptr;
  B().device->CreateSamplerState(&s, &out);
  sampler_cache_[key] = out;
  return out;
}

void Device::ApplyState(bool tl, bool depth_clip, bool has_depth) {
  Backend& b = B();
  b.ctx->OMSetBlendState(GetBlend(), nullptr, 0xffffffff);
  b.ctx->OMSetDepthStencilState(GetDepth(has_depth), 0);
  b.ctx->RSSetState(GetRaster(depth_clip));

  // Textures and samplers.
  ID3D11ShaderResourceView* srv[2] = {};
  ID3D11SamplerState* smp[3] = {};
  PSConstants pc{};
  for (int s = 0; s < kStages; ++s) {
    const DWORD* t = tss_[s];
    pc.scol[s][0] = static_cast<int32_t>(t[TSS_COLOROP]);
    pc.scol[s][1] = static_cast<int32_t>(t[TSS_COLORARG1]);
    pc.scol[s][2] = static_cast<int32_t>(t[TSS_COLORARG2]);
    pc.salp[s][0] = static_cast<int32_t>(t[TSS_ALPHAOP]);
    pc.salp[s][1] = static_cast<int32_t>(t[TSS_ALPHAARG1]);
    pc.salp[s][2] = static_cast<int32_t>(t[TSS_ALPHAARG2]);
    if (t[TSS_COLOROP] > 16) LogOnce("texture op %lu not emulated", t[TSS_COLOROP]);
    if (tex_[s] && t[TSS_COLOROP] != D3DTOP_DISABLE) {
      if (tex_[s] == rt_) {
        LogOnce("texture is the current render target; ignored");
      } else {
        srv[s] = tex_[s]->SampleSrv();
        pc.scol[s][3] = srv[s] ? 1 : 0;
      }
    }
    smp[s] = GetSampler(s);
  }
  smp[2] = g_pipe.key_sampler;
  b.ctx->PSSetShaderResources(0, 2, srv);
  b.ctx->PSSetSamplers(0, 3, smp);

  ColorToFloat(rs_[RS_TEXTUREFACTOR], pc.tfactor);
  ColorToFloat(rs_[RS_FOGCOLOR], pc.fogcolor);
  pc.fogcolor[3] = 1;
  if (rs_[RS_FOGENABLE]) {
    DWORD mode = rs_[RS_FOGTABLEMODE] ? rs_[RS_FOGTABLEMODE] : rs_[RS_FOGVERTEXMODE];
    // ini FogDistance pushes the fog out (or pulls it in).
    const float scale = GetConfig().fog_scale;
    pc.fog[0] = AsFloat(rs_[RS_FOGSTART]) * scale;
    pc.fog[1] = AsFloat(rs_[RS_FOGEND]) * scale;
    pc.fog[2] = AsFloat(rs_[RS_FOGDENSITY]) / scale;
    pc.fog[3] = static_cast<float>(mode & 3);
    LogOnce("fog mode %lu/%lu start %.1f end %.1f density %.4f", rs_[RS_FOGTABLEMODE],
            rs_[RS_FOGVERTEXMODE], AsFloat(rs_[RS_FOGSTART]), AsFloat(rs_[RS_FOGEND]),
            AsFloat(rs_[RS_FOGDENSITY]));
  }
  pc.atest[0] = static_cast<float>(rs_[RS_ALPHAREF] & 0xff);
  pc.atest[1] = rs_[RS_ALPHATESTENABLE] ? static_cast<float>(rs_[RS_ALPHAFUNC]) : 8.0f;
  pc.atest[2] = rs_[RS_SPECULARENABLE] ? 1.0f : 0.0f;
  pc.atest[3] = rs_[RS_COLORKEYENABLE] ? 1.0f : 0.0f;
  if (rs_[RS_COLORKEYENABLE] && tex_[0] && tex_[0]->HasColorKey()) {
    tex_[0]->KeyColor(pc.key0);
    pc.key0[3] = 1;
  }
  // Blended cut-outs (leaves, flags) that also write depth hide whatever is
  // drawn behind them later, even where they are nearly transparent; the
  // original game shows a stair-step fringe there. Drop those pixels.
  if (rs_[RS_ALPHABLENDENABLE] && rs_[RS_ZWRITEENABLE] && rs_[RS_ZENABLE] &&
      (rs_[RS_SRCBLEND] == 5 || rs_[RS_SRCBLEND] == 12) &&  // SRCALPHA, BOTHSRCALPHA
      // The game marks its cut-outs with alpha test GREATER 0, which lets
      // every filtered edge pixel through; soft layers (terrain detail,
      // shadows) have alpha test off and are left alone.
      rs_[RS_ALPHATESTENABLE] && (rs_[RS_ALPHAFUNC] == 5 || rs_[RS_ALPHAFUNC] == 7))
    pc.cut[0] = static_cast<float>(GetConfig().alpha_cut) / 255.0f;
  b.ctx->UpdateSubresource(ps_cb_, 0, nullptr, &pc, 0, 0);

  bool flat = rs_[RS_SHADEMODE] == 1;
  b.ctx->IASetInputLayout(g_pipe.layout);
  b.ctx->VSSetShader(g_pipe.vs[flat], nullptr, 0);
  b.ctx->PSSetShader(g_pipe.ps[flat], nullptr, 0);
  b.ctx->VSSetConstantBuffers(0, 1, &vs_cb_);
  b.ctx->PSSetConstantBuffers(1, 1, &ps_cb_);
  (void)tl;
}

void Device::Submit(const Draw& d) {
  bool has_depth = false;
  if (!BindTarget(has_depth)) return;
  Backend& b = B();
  const FvfLayout& l = d.layout;
  if (!l.stride) return;

  // Topology, and fans turned into lists.
  D3D11_PRIMITIVE_TOPOLOGY topo;
  switch (d.prim) {
    case D3DPT_POINTLIST: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case D3DPT_LINELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case D3DPT_LINESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case D3DPT_TRIANGLESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case D3DPT_TRIANGLELIST:
    case D3DPT_TRIANGLEFAN: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    default: LogOnce("primitive type %lu unsupported", d.prim); return;
  }
  const WORD* idx = d.idx;
  DWORD nidx = d.nidx;
  if (d.prim == D3DPT_TRIANGLEFAN) {
    DWORD n = idx ? nidx : d.count;
    if (n < 3) return;
    scratch_idx_.resize((n - 2) * 3);
    for (DWORD i = 1; i + 1 < n; ++i) {
      scratch_idx_[(i - 1) * 3 + 0] = idx ? idx[0] : 0;
      scratch_idx_[(i - 1) * 3 + 1] = idx ? idx[i] : static_cast<WORD>(i);
      scratch_idx_[(i - 1) * 3 + 2] = idx ? idx[i + 1] : static_cast<WORD>(i + 1);
    }
    idx = scratch_idx_.data();
    nidx = static_cast<DWORD>(scratch_idx_.size());
  }

  // Which vertex mode: untransformed, screen space, or clip space from
  // ProcessVertices.
  int mode = l.tl ? 1 : 0;
  if (l.tl && d.vb && d.vb->HasClipData(d.first, d.count)) mode = 2;
  if (!l.tl && rs_[RS_LIGHTING] && l.normal >= 0) LogOnce("lit vertices drawn unlit (fvf has normals)");

  int set0 = static_cast<int>(tss_[0][TSS_TEXCOORDINDEX] & 0xffff);
  int set1 = static_cast<int>(tss_[1][TSS_TEXCOORDINDEX] & 0xffff);
  scratch_.resize(static_cast<size_t>(d.count) * sizeof(GpuVertex));
  auto* out = reinterpret_cast<GpuVertex*>(scratch_.data());
  const VertexBuffer::Processed* clip = mode == 2 ? d.vb->ClipData() : nullptr;
  for (DWORD i = 0; i < d.count; ++i) {
    const uint8_t* s = d.verts + static_cast<size_t>(d.first + i) * l.stride;
    GpuVertex& v = out[i];
    if (clip) {
      std::memcpy(v.pos, clip[d.first + i].clip, 16);
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
  UINT vbytes = d.count * sizeof(GpuVertex);
  if (vbytes > vbuf_size_) {
    LogOnce("draw of %lu vertices too large", d.count);
    return;
  }
  UINT voff = PushVertices(scratch_.data(), vbytes);
  if (voff == UINT(-1)) return;
  UINT ioff = 0;
  if (idx) {
    if (nidx * 2 > ibuf_size_) return;
    ioff = PushIndices(idx, nidx * 2);
    if (ioff == UINT(-1)) return;
  }

  // Constants and viewport.
  const D3DVIEWPORT7& vp7 = mode == 2 ? d.vb->ClipViewport() : viewport_;
  VSConstants vc{};
  std::memcpy(vc.wvp, Wvp(), sizeof(vc.wvp));
  vc.vp[0] = static_cast<float>(vp7.dwX);
  vc.vp[1] = static_cast<float>(vp7.dwY);
  vc.vp[2] = static_cast<float>(vp7.dwWidth ? vp7.dwWidth : 1);
  vc.vp[3] = static_cast<float>(vp7.dwHeight ? vp7.dwHeight : 1);
  vc.mode[0] = static_cast<float>(mode);
  b.ctx->UpdateSubresource(vs_cb_, 0, nullptr, &vc, 0, 0);
  D3D11_VIEWPORT v{vc.vp[0], vc.vp[1], vc.vp[2], vc.vp[3], 0.0f, 1.0f};
  if (mode != 1) {
    v.MinDepth = vp7.dvMinZ;
    v.MaxDepth = vp7.dvMaxZ;
  }
  b.ctx->RSSetViewports(1, &v);
  ApplyState(l.tl, mode != 1, has_depth);

  UINT stride = sizeof(GpuVertex);
  b.ctx->IASetVertexBuffers(0, 1, &vbuf_, &stride, &voff);
  b.ctx->IASetPrimitiveTopology(topo);
  if (idx) {
    b.ctx->IASetIndexBuffer(ibuf_, DXGI_FORMAT_R16_UINT, ioff);
    b.ctx->DrawIndexed(nidx, 0, 0);
  } else {
    b.ctx->Draw(d.count, 0);
  }
  if (cur_msaa_) MsaaMarkDrawn();
  rt_->MarkGpuWritten();
  ++Counters().draws;
}

}  // namespace dd
