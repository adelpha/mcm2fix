#include "../prof.h"
#include "../smooth.h"
#include "device.h"

#include "../simd.h"
#include "drawlog.h"
#include "motionblur.h"
#include "gpushadow.h"
#include "gputext.h"
#include "hires3d.h"
#include "hudtext.h"
#include "racetitle.h"
#include "probe.h"
#include "reflect.h"
#include "../fartrees.h"
#include "../gputerrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "../config.h"

void WalkSceneEnd();  // fixes_walk.cpp
#include "directdraw.h"
#include "msaa.h"
#include "surface.h"

namespace dd {
unsigned g_devices_made = 0, g_devices_gone = 0;

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
                  // y: alpha-to-coverage, 1 sharpened at x, 2 alpha as is
  float4 tfar;    // GpuTerrain near/far split: x distance (0 off), y focal (pixels), zw screen centre
  float4 tfard;   // its far diffuse colour
  float4 tfars;   // its far specular colour
};
#ifdef SHADOW
// Rider shadows on the terrain (gpushadow.cpp): each pixel's light-space
// position is looked up in the riders' tiles of the shadow atlas. The atlas
// holds coverage (r) and coverage times the casters' lowest surface below
// their top (g), with mipmaps: the further the ground lies below the caster
// over it, the blurrier the level read and the fainter the shadow.
cbuffer ShadowParams : register(b2) {
  row_major float4x4 lsm;  // world -> light space: x, y across the sun's view, z along its rays
  float4 sinfo;            // x shadows, y strength, z atlas texel (uv), w reach below the casters' top
  float4 ssoft;            // x blur per unit of height (tile texels at level 0 per 1/half), y fade per unit, z top level
  float4 stile[16];        // light-space centre xy, 1 / half size, casters' top (smallest z)
};
Texture2D satlas : register(t3);
SamplerState ssmp : register(s3);
float ShadowCover(float3 ls) {
  float cov = 0;
  int n = (int)sinfo.x;
  [loop] for (int k = 0; k < n; ++k) {
    float4 t = stile[k];
    float2 l = (ls.xy - t.xy) * t.z;
    if (max(abs(l.x), abs(l.y)) < 0.97 && ls.z > t.w && ls.z < t.w + sinfo.w) {
      float2 cell = float2(k & 3, k >> 2);
      float2 uv = (cell + float2(0.5 + 0.5 * l.x, 0.5 - 0.5 * l.y)) * 0.25;
      // How far below the casters over this spot the ground is.
      float2 bl = satlas.SampleLevel(ssmp, uv, ssoft.z).rg;
      float below = bl.x > 0.002 ? bl.y / bl.x : 0;
      float gap = max(ls.z - t.w - below, 0);
      float lod = clamp(log2(1 + gap * ssoft.x * t.z), 0, ssoft.z);
      float o = sinfo.z * (lod > 0.5 ? 0.5 * exp2(lod) : 0.75);
      float c = satlas.SampleLevel(ssmp, uv + float2(-o, -o), lod).r + satlas.SampleLevel(ssmp, uv + float2(o, -o), lod).r +
                satlas.SampleLevel(ssmp, uv + float2(-o, o), lod).r + satlas.SampleLevel(ssmp, uv + float2(o, o), lod).r;
      cov = max(cov, c * 0.25 / (1 + gap * ssoft.y));
    }
  }
  return cov;
}
#endif
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState smp0 : register(s0);
SamplerState smp1 : register(s1);
SamplerState keysmp : register(s2);
Texture2D tex2 : register(t6);  // GpuTerrain: the parent's whole texture (far away)
SamplerState smp2 : register(s4);
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
  float pw : TEXCOORD4;
#ifdef SHADOW
  float3 ls : TEXCOORD3;
#endif
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
  o.pw = i.pos.w;
  o.diff = i.diff;
  o.spec = i.spec;
  o.t0 = i.t0;
  o.t1 = i.t1;
#ifdef SHADOW
  o.ls = mul(float4(i.pos.xyz, 1), lsm).xyz;
#endif
  return o;
}
#ifdef TREES
// FarTrees (fartrees.cpp): the game's far tree billboards, made here from
// each tree's fixed numbers and the frame's camera. Same quad, texture
// coordinates, colour and culling as the game's builder (0x45be20).
Buffer<float4> ftrees : register(t4);
cbuffer TreeParams : register(b3) {
  float4 tr0;  // right x, right z, view _13, view _33
  float4 tr1;  // view _43, key scale, key unit, far depth (minus eps)
  float4 tr2;  // max depth, focal, min size, six_ac
  float4 tr3;  // six on, six_84, six near, eps
  float4 tcol;
  float4 tspec;
};
VSOut vs_tree(uint id : SV_VertexID) {
  VSOut o;
  uint t = id / 6, c = id % 6;
  uint corner = c < 3 ? c : (c == 3 ? 0 : c - 2);  // (0 1 2) (0 2 3)
  float4 a = ftrees.Load(t * 3);      // x, z, bottom y, top y
  float4 b = ftrees.Load(t * 3 + 1);  // left, right, H, -
  float4 u = ftrees.Load(t * 3 + 2);  // u right, u left, v top, v bottom
  float v = a.x * tr0.z + a.y * tr0.w + tr1.x;
  float key = v * tr1.y;
  key = key < 0 ? 0 : min(floor(key), 65535);
  float depth = key * tr1.z;
  bool six = tr3.x != 0 && ((tr3.y != 0 && depth < tr3.z) || (depth + tr3.w) * tr2.w < b.z);
  bool draw = depth > tr1.w && depth < tr2.x && b.z * tr2.y / max(depth, 1e-6) > tr2.z && !six;
  bool rside = corner == 0 || corner == 3;
  float side = rside ? b.y : -b.x;
  float3 pos = float3(a.x + side * tr0.x, corner < 2 ? a.z : a.w, a.y - side * tr0.y);
  float4 p = mul(float4(pos, 1), wvp);
  p.x += p.w / vp.z;
  p.y -= p.w / vp.w;
  o.pos = draw ? p : float4(0, 0, -2, 1);
  o.fogw = p.w;
  o.pw = 0;
  o.diff = tcol;
  o.spec = tspec;
  o.t0 = float2(rside ? u.x : u.y, corner < 2 ? u.w : u.z);
  o.t1 = o.t0;
  return o;
}
#endif
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
  // GpuTerrain's detail texture fades out between acut.z and acut.w (depth).
  if (acut.w > 0) tc1.a *= saturate((acut.w - i.fogw) / max(acut.w - acut.z, 0.001));
  if (scol[0].w == 0) tc0 = white;
  if (scol[1].w == 0) tc1 = white;
  // GpuTerrain: beyond the game's near/far distance (distance from the eye,
  // from depth and the pixel's offset from the screen centre), as its far
  // blocks: no detail, far diffuse and specular colours.
  bool tfar_on = false;
  if (tfar.x > 0) {
    float2 off = (i.pos.xy - tfar.zw) / tfar.y;
    float dist = i.fogw * sqrt(1 + dot(off, off));
    // Sampled outside the varying branches below (gradients).
    float k = max(round(i.pw) - 1, 0);
    float2 place = float2(fmod(k, 16), floor(k / 16));
    float4 tcw = tex2.Sample(smp2, (i.t1 * 12 / 256 + place / 16) * 0.9375 + 0.03125);
    if (dist > tfar.x) {
      tfar_on = true;
      tc1.a = 0;
      i.diff = tfard;
      // Further still, level 0 blocks on their parent's whole texture
      // (1/256 of it per cell; cell index from the detail uv, 1/12 per cell).
      if (tfars.w > 0 && dist > tfars.w && i.pw > 0.5) tc0 = tcw;
    }
  }
  float4 cur = i.diff;
  if (scol[0].x != 1) {
    cur = Stage(0, i, cur, tc0);
    if (scol[1].x != 1) cur = Stage(1, i, cur, tc1);
  }
  // Screen-space rate of change of alpha, for sharpened alpha-to-coverage;
  // taken before any discard so the derivative is defined.
  float aw = max(fwidth(cur.a), 1.0 / 255);
  if (atest.w != 0 && key0.w != 0) {
    float3 k = tex0.Sample(keysmp, i.t0).rgb;
    if (all(abs(k - key0.rgb) < 0.002)) discard;
  }
#ifdef SHADOW
  cur.rgb *= 1 - ShadowCover(i.ls) * sinfo.y;
#endif
  if (atest.z != 0) cur.rgb = saturate(cur.rgb + i.spec.rgb);
  if (tfar_on) cur.rgb = saturate(cur.rgb + tfars.rgb);
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
  if (acut.y == 1) {
    // Alpha-to-coverage, sharpened: the edge sits where alpha crosses the
    // cut and is about one pixel wide at any distance or magnification; the
    // MSAA samples covered give the antialiased edge.
    cur.a = saturate((cur.a - acut.x) / aw + 0.5);
  } else if (acut.y == 0) {
    if (cur.a < acut.x) discard;
  }
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
  float tfar[4];
  float tfard[4];
  float tfars[4];
};

// What the Device last bound on the D3D11 context (ini StateCache=1), so
// unchanged state is not sent again for every draw. Anything else that
// changes bindings calls ContextTouched (backend.h), which makes it stale.
struct Bound {
  const void* owner = nullptr;
  unsigned epoch = 0;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11BlendState* blend = nullptr;
  ID3D11DepthStencilState* depth = nullptr;
  ID3D11RasterizerState* raster = nullptr;
  ID3D11ShaderResourceView* srv[2] = {};
  ID3D11SamplerState* smp[3] = {};
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  bool pipe = false;  // layout, constant buffers, vertex and index buffers
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  D3D11_VIEWPORT vp{};
  bool vp_ok = false;
  VSConstants vc{};
  bool vc_ok = false;
  PSConstants pc{};
  bool pc_ok = false;
};
Bound g_bound;
struct CacheStats {
  unsigned draws = 0, resets = 0, targets = 0, states = 0, textures = 0, vs_consts = 0, ps_consts = 0;
} g_cstat;

// The cache for this device, emptied if anything else used the context.
Bound& Cache(const void* dev) {
  if (!GetConfig().state_cache || g_bound.owner != dev || g_bound.epoch != ContextEpoch()) {
    g_bound = Bound{};
    g_bound.owner = dev;
    g_bound.epoch = ContextEpoch();
    ++g_cstat.resets;
  }
  return g_bound;
}

void ReportCache() {
  static DWORD last = GetTickCount();
  const DWORD now = GetTickCount();
  if (now - last < 5000) return;
  last = now;
  if ((GetConfig().profile || GetConfig().dd_stats) && g_cstat.draws)
    Log("state cache 5s (StateCache=%d): %u draws, %u full rebinds, set: %u targets, %u blend/depth/raster, "
        "%u texture/sampler, %u vs constants, %u ps constants",
        GetConfig().state_cache ? 1 : 0, g_cstat.draws, g_cstat.resets, g_cstat.targets, g_cstat.states,
        g_cstat.textures, g_cstat.vs_consts, g_cstat.ps_consts);
  g_cstat = CacheStats{};
}

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
  // Terrain with rider shadows (gpushadow.cpp), [flat].
  ID3D11VertexShader* vs_shadow[2] = {};
  ID3D11PixelShader* ps_shadow[2] = {};
  bool shadow_ok = false;
  // FarTrees, [flat].
  ID3D11VertexShader* vs_tree[2] = {};
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
  // The terrain variant with rider shadows; without it the terrain is drawn
  // without them.
  g_pipe.shadow_ok = true;
  for (int flat = 0; flat < 2; ++flat) {
    std::string src = std::string("#define SHADOW\n") + (flat ? "#define FLAT\n" : "") + kShader;
    ID3DBlob* vs = Compile(src.c_str(), "vs_main", "vs_4_0");
    ID3DBlob* ps = Compile(src.c_str(), "ps_main", "ps_4_0");
    if (vs) b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_pipe.vs_shadow[flat]);
    if (ps) b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_pipe.ps_shadow[flat]);
    SafeRelease(vs);
    SafeRelease(ps);
    if (!g_pipe.vs_shadow[flat] || !g_pipe.ps_shadow[flat]) g_pipe.shadow_ok = false;
  }
  Log("rider shadows: terrain shader variant %s", g_pipe.shadow_ok ? "ready" : "FAILED (no GPU shadows)");
  for (int flat = 0; flat < 2; ++flat) {
    std::string src = std::string("#define TREES\n") + (flat ? "#define FLAT\n" : "") + kShader;
    ID3DBlob* vs = Compile(src.c_str(), "vs_tree", "vs_4_0");
    if (vs) b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_pipe.vs_tree[flat]);
    SafeRelease(vs);
  }
  Log("far trees: shader %s", g_pipe.vs_tree[0] && g_pipe.vs_tree[1] ? "ready" : "FAILED (FarTrees off)");
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
  // Remember the matrix (the D3D9 renderer redraws these vertices from their
  // object-space positions so the GPU can clip them).
  if (mats_.empty() || std::memcmp(mats_.back().m, m, sizeof(Mat)) != 0) {
    if (mats_.size() >= 4096) {  // start over; older ids become stale (see Matrix)
      mats_.clear();
      ++mat_epoch_;
    }
    Mat mm;
    std::memcpy(mm.m, m, sizeof(mm.m));
    mats_.push_back(mm);
  }
  const uint32_t mat_id = static_cast<uint32_t>(mats_.size() - 1) | (mat_epoch_ << 12);
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
      std::memcpy(out.obj, p, 12);
      out.mat = mat_id;
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

namespace {
Device* g_batch_owner = nullptr;
Device* g_tree_device = nullptr;  // FarTrees: the device of the latest scene  // the device with merged draws waiting (DrawBatch)
}  // namespace

// ------------------------------------------------------------ draw batching
//
// DrawBatch=1. The game sends many small 3D draws in a row with nothing
// changed between them (every near tree is its own draw of a few dozen
// vertices, already in world space). Each one costs a vertex conversion, a
// state check and a driver call. Here they are collected, as long as nothing
// that affects drawing changes, and sent as one triangle list. Anything that
// could change what a waiting draw looks like, or read what it drew (a state
// change, a texture or render target being written or read, a screen-space
// draw, the end of the scene) sends the waiting draws first.

namespace {

constexpr DWORD kBatchMaxVerts = 16384;
constexpr size_t kBatchMaxIdx = 49152;

const char* const kBreakNames[kBreakCount] = {"render state", "stage state", "texture", "transform", "viewport",
                                              "render target", "material/light", "clear", "begin/end scene",
                                              "surface access", "flip", "screen-space draw", "not mergeable",
                                              "vertex format", "full", "other"};

struct BatchStats {
  unsigned seen = 0;       // 3D draws from the game
  unsigned merged = 0;     // of them, taken into a batch
  unsigned sent = 0;       // batches drawn
  unsigned single = 0;     // batches of one draw
  unsigned max_draws = 0;
  unsigned not_mergeable[6] = {};  // prim (points/lines), processed verts, index range, too big, fvf, off
  unsigned breaks[kBreakCount] = {};
  std::map<unsigned, unsigned> rs_breaks, tss_breaks;  // which state split batches (tss: stage/type | old << 8 | new << 16)
  unsigned tss_kept = 0;                               // stage changes that needed no flush (unused setting)
  std::map<unsigned, unsigned> tss_kept_by;            // stage/type
} g_bs;

void NoteStageStateKept(DWORD stage, DWORD type) {
  ++g_bs.tss_kept;
  ++g_bs.tss_kept_by[stage * 32 + type];
}


bool SameLayout(const FvfLayout& a, const FvfLayout& b) {
  if (a.stride != b.stride || a.tl != b.tl || a.diffuse != b.diffuse || a.specular != b.specular ||
      a.normal != b.normal || a.ntex != b.ntex)
    return false;
  for (int i = 0; i < a.ntex && i < 8; ++i)
    if (a.tex[i] != b.tex[i] || a.tex_dim[i] != b.tex_dim[i]) return false;
  return true;
}

void ReportBatches() {
  static DWORD last = GetTickCount();
  const DWORD now = GetTickCount();
  if (now - last < 5000) return;
  const double secs = (now - last) / 1000.0;
  last = now;
  const Config& c = GetConfig();
  if (g_bs.seen && (c.batch_log || c.profile || c.dd_stats || c.draw_log)) {
    std::string why;
    for (int i = 0; i < kBreakCount; ++i)
      if (g_bs.breaks[i]) {
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s %u", why.empty() ? "" : ", ", kBreakNames[i], g_bs.breaks[i]);
        why += b;
      }
    auto top = [](const std::map<unsigned, unsigned>& m, bool tss) {
      std::vector<std::pair<unsigned, unsigned>> v(m.begin(), m.end());
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string out;
      for (size_t i = 0; i < v.size() && i < 6; ++i) {
        char b[48];
        if (tss && v[i].first > 0xff)
          std::snprintf(b, sizeof(b), " %u/%u %u->%u x%u", (v[i].first >> 5) & 7, v[i].first & 31,
                        (v[i].first >> 8) & 0xff, v[i].first >> 16, v[i].second);
        else if (tss) std::snprintf(b, sizeof(b), " %u/%u x%u", v[i].first >> 5, v[i].first & 31, v[i].second);
        else std::snprintf(b, sizeof(b), " %u x%u", v[i].first, v[i].second);
        out += b;
      }
      return out.empty() ? std::string(" -") : out;
    };
    Log("draw batching %.1fs: %u 3D draws, %u merged into %u batches (%u of one draw, most %u draws); "
        "%u draws drawn for them instead of %u; not mergeable: points/lines %u, processed %u, index range %u, "
        "too big %u, fan/strip too short %u",
        secs, g_bs.seen, g_bs.merged, g_bs.sent, g_bs.single, g_bs.max_draws,
        g_bs.sent + (g_bs.seen - g_bs.merged), g_bs.seen, g_bs.not_mergeable[0], g_bs.not_mergeable[1],
        g_bs.not_mergeable[2], g_bs.not_mergeable[3], g_bs.not_mergeable[4]);
    Log("draw batching %.1fs: batches ended by %s; render states that changed:%s; stage/state old->new that "
        "changed:%s; stage changes kept in the batch (setting not used) %u:%s",
        secs, why.empty() ? "-" : why.c_str(), top(g_bs.rs_breaks, false).c_str(), top(g_bs.tss_breaks, true).c_str(),
        g_bs.tss_kept, top(g_bs.tss_kept_by, true).c_str());
  }
  g_bs = BatchStats{};
}

}  // namespace

// A texture stage setting that can't change what the waiting draws look
// like, so changing it needs no flush: stages past the first disabled one
// (both renderers stop there), anything but the colour op on that disabled
// stage, and the argument a "select" op or a disabled alpha op doesn't read.
// Whatever later makes it matter again is an op change, which does flush, and
// then the waiting draws are drawn with the new argument under the old op,
// which ignores it.
bool Device::StageStateUnused(DWORD stage, DWORD type) const {
  if (!GetConfig().draw_batch) return false;
  if (stage >= static_cast<DWORD>(kStages)) return true;
  DWORD first_off = kStages;
  for (DWORD k = 0; k < static_cast<DWORD>(kStages); ++k)
    if (tss_[k][TSS_COLOROP] == D3DTOP_DISABLE) {
      first_off = k;
      break;
    }
  if (stage > first_off) return true;
  if (stage == first_off) return type != TSS_COLOROP;
  const DWORD cop = tss_[stage][TSS_COLOROP], aop = tss_[stage][TSS_ALPHAOP];
  constexpr DWORD kSel1 = 2, kSel2 = 3;
  if (type == TSS_COLORARG1) return cop == kSel2;
  if (type == TSS_COLORARG2) return cop == kSel1;
  if (type == TSS_ALPHAARG1) return aop == kSel2 || aop == D3DTOP_DISABLE;
  if (type == TSS_ALPHAARG2) return aop == kSel1 || aop == D3DTOP_DISABLE;
  return false;
}

void FlushPendingDraws(BatchBreak why) {
  if (g_batch_owner) g_batch_owner->FlushBatch(why);
}

bool Device::BatchAdd(const Draw& d) {
  if (!GetConfig().draw_batch) return false;
  ++g_bs.seen;
  ReportBatches();
  auto refuse = [&](int reason) {
    ++g_bs.not_mergeable[reason];
    FlushBatch(kBreakShape);
    return false;
  };
  if (d.prim != D3DPT_TRIANGLELIST && d.prim != D3DPT_TRIANGLEFAN && d.prim != D3DPT_TRIANGLESTRIP) return refuse(0);
  if (d.layout.tl || !d.verts || !d.layout.stride) return refuse(1);
  if (d.count > kBatchMaxVerts) return refuse(3);
  const DWORD n = d.idx ? d.nidx : d.count;  // vertices of the primitive, through the indices if any
  if (n < 3) return refuse(4);
  if (d.idx) {
    WORD hi = 0;
    for (DWORD i = 0; i < d.nidx; ++i) hi = std::max(hi, d.idx[i]);
    if (hi >= d.count) return refuse(2);
  }
  const size_t tris = d.prim == D3DPT_TRIANGLELIST ? n / 3 : n - 2;
  if (tris * 3 > kBatchMaxIdx) return refuse(3);
  const bool receiver = ShadowReceiverNow();
  if (batch_.draws) {
    if (batch_.receiver != receiver) FlushBatch(kBreakOther);
    else if (!SameLayout(batch_.layout, d.layout)) FlushBatch(kBreakLayout);
    else if (batch_.nverts + d.count > kBatchMaxVerts || batch_.idx.size() + tris * 3 > kBatchMaxIdx)
      FlushBatch(kBreakFull);
  }
  if (!batch_.draws) {
    batch_.layout = d.layout;
    batch_.receiver = receiver;
    batch_.verts.clear();
    batch_.idx.clear();
    batch_.subs.clear();
    batch_.nverts = 0;
  }
  const size_t stride = d.layout.stride;
  const uint8_t* src = d.verts + static_cast<size_t>(d.first) * stride;
  batch_.verts.insert(batch_.verts.end(), src, src + static_cast<size_t>(d.count) * stride);
  const WORD base = static_cast<WORD>(batch_.nverts);
  auto at = [&](DWORD i) -> WORD { return static_cast<WORD>(base + (d.idx ? d.idx[i] : static_cast<WORD>(i))); };
  const size_t o = batch_.idx.size();
  batch_.idx.resize(o + tris * 3);
  WORD* out = batch_.idx.data() + o;
  if (d.prim == D3DPT_TRIANGLELIST) {
    for (size_t t = 0; t < tris * 3; ++t) out[t] = at(static_cast<DWORD>(t));
  } else if (d.prim == D3DPT_TRIANGLEFAN) {
    for (size_t t = 0; t < tris; ++t) {
      out[t * 3] = at(0);
      out[t * 3 + 1] = at(static_cast<DWORD>(t + 1));
      out[t * 3 + 2] = at(static_cast<DWORD>(t + 2));
    }
  } else {  // strip: every other triangle turned round, as the GPU draws them
    for (size_t t = 0; t < tris; ++t) {
      const DWORD a = static_cast<DWORD>(t);
      out[t * 3] = at(t & 1 ? a + 1 : a);
      out[t * 3 + 1] = at(t & 1 ? a : a + 1);
      out[t * 3 + 2] = at(a + 2);
    }
  }
  batch_.subs.push_back({batch_.nverts, d.count, static_cast<DWORD>(o), static_cast<DWORD>(tris * 3)});
  batch_.nverts += d.count;
  ++batch_.draws;
  ++g_bs.merged;
  g_batch_owner = this;
  return true;
}

void Device::FlushBatch(BatchBreak why) {
  if (!batch_.draws || flushing_) return;
  DD_LOCK;
  ++g_bs.breaks[why];
  ++g_bs.sent;
  if (batch_.draws == 1) ++g_bs.single;
  g_bs.max_draws = std::max(g_bs.max_draws, batch_.draws);
  // Drawing may come back here (a surface it touches): send from a copy.
  std::swap(batch_, batch_out_);
  batch_.draws = 0;
  if (g_batch_owner == this) g_batch_owner = nullptr;
  flushing_ = true;
  Draw d;
  d.prim = D3DPT_TRIANGLELIST;
  d.verts = batch_out_.verts.data();
  d.layout = batch_out_.layout;
  d.first = 0;
  d.count = batch_out_.nverts;
  d.idx = batch_out_.idx.data();
  d.nidx = static_cast<DWORD>(batch_out_.idx.size());
  SubmitDraw(d);
  flushing_ = false;
}


Device::Device(DirectDraw* dd, Surface* rt, REFCLSID cls) : dd_(dd), rt_(rt), guid_(cls) {
  ++g_devices_made;
  dd_->AddRef();
  rt_->Hold();
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
  ++g_devices_gone;
  if (g_batch_owner == this) g_batch_owner = nullptr;  // waiting draws are dropped with the device
  for (Surface*& t : tex_)
    if (t) {
      t->Unhold();
      t = nullptr;
    }
  if (rt_) rt_->Unhold();
  rt_ = nullptr;
  ReflectForget();  // the reflection cache holds this device's states
  for (auto& kv : blend_cache_) kv.second->Release();
  for (auto& kv : depth_cache_) kv.second->Release();
  for (auto& kv : raster_cache_) kv.second->Release();
  for (auto& kv : sampler_cache_) kv.second->Release();
  SafeRelease(vbuf_);
  SafeRelease(ibuf_);
  SafeRelease(vs_cb_);
  SafeRelease(ps_cb_);
  SafeRelease(tree_srv_);
  SafeRelease(tree_buf_);
  SafeRelease(tree_cb_);
  SafeRelease(terr_vb_);
  SafeRelease(terr_ib_);
  if (g_tree_device == this) g_tree_device = nullptr;
  if (g_bound.owner == this) g_bound = Bound{};
  dd_->Release();
}

bool Device::Init() {
  if (IsD3D9()) return Init9();
  g_bound = Bound{};
  if (!InitPipeline()) return false;
  ReflectInstallHooks();
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
  // TrueColorTextures adds 24-bit RGB and 32-bit ARGB (the game's ids 0x378
  // and 0x22b8, see fixes_textures.cpp). Every format ends up as BGRA8 on
  // the GPU; these only decide how much colour the game keeps when it
  // converts its pictures.
  const bool true_color = GetConfig().true_color_textures;
  DDPIXELFORMAT formats[] = {
      MakeRGBFormat(16, 0xf800, 0x07e0, 0x001f, 0),
      MakeRGBFormat(16, 0x7c00, 0x03e0, 0x001f, 0),
      MakeRGBFormat(16, 0x7c00, 0x03e0, 0x001f, 0x8000),
      MakeRGBFormat(16, 0x0f00, 0x00f0, 0x000f, 0xf000),
      MakeRGBFormat(24, 0xff0000, 0x00ff00, 0x0000ff, 0),
      MakeRGBFormat(32, 0xff0000, 0x00ff00, 0x0000ff, 0xff000000),
  };
  const size_t count = true_color ? 6 : 4;
  LogOnce("EnumTextureFormats: offering %u formats (TrueColorTextures=%d)",
          static_cast<unsigned>(count), true_color ? 1 : 0);
  for (size_t i = 0; i < count; ++i)
    if (cb(&formats[i], ctx) == D3DENUMRET_CANCEL) break;
  return D3D_OK;
}

HRESULT Device::BeginScene() {
  FlushBatch(kBreakScene);
  g_tree_device = this;
  return D3D_OK;
}
HRESULT Device::EndScene() {
  FlushBatch(kBreakScene);
  ProbeEndScene();
  WalkSceneEnd();  // FastObjectWalk: the scene's object list is done
  if (ReflectMode() && !IsD3D9()) {
    PROF_ZONE(kReflect);
    ReflectEndScene(g_pipe.layout, sizeof(GpuVertex));
  }
  if (MotionBlurOn()) MotionBlurReplay();  // the arrow, if no HUD draw came after the scene
  RaceTitleDraw(rt_);  // countdown race title, under the HUD panels' text
  HudTextFlush();  // race HUD text goes over everything the scene drew
  return D3D_OK;
}

HRESULT Device::GetDirect3D(IDirect3D7** out) {
  if (!out) return DDERR_INVALIDPARAMS;
  *out = dd_->D3D();
  (*out)->AddRef();
  return D3D_OK;
}

HRESULT Device::SetRenderTarget(IDirectDrawSurface7* s, DWORD) {
  DD_LOCK;
  FlushBatch(kBreakTarget);
  Surface* t = Surface::From(s);
  if (!t) return DDERR_INVALIDPARAMS;
  t->Hold();
  t->SetUsedAsRenderTarget();
  if (rt_) rt_->Unhold();
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
  FlushBatch(kBreakClear);
  PROF_ZONE(kClear);
  prof::Count(prof::cClears);
  const bool hi = HiResPrepare();
  if (rt_ && (flags & D3DCLEAR_TARGET)) {
    const void* n = rt_->GpuKey();
    if (GpuTextHasText(n)) {
      RECT vp{static_cast<LONG>(viewport_.dwX), static_cast<LONG>(viewport_.dwY),
              static_cast<LONG>(viewport_.dwX + viewport_.dwWidth), static_cast<LONG>(viewport_.dwY + viewport_.dwHeight)};
      if (!count || !rects) {
        GpuTextOcclude(n, &vp, "clear");
      } else {
        for (DWORD i = 0; i < count; ++i) {
          RECT r{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2}, o;
          if (IntersectRect(&o, &r, &vp)) GpuTextOcclude(n, &o, "clear");
        }
      }
    }
  }
  if (IsD3D9()) return Clear9(count, rects, flags, color, z);
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
    ContextTouched();
  }
  // MenuHiRes3D: the same clears in the window-sized copy.
  ID3D11RenderTargetView* hi_rtv = nullptr;
  ID3D11DepthStencilView* hi_dsv = nullptr;
  float hsx = 1, hsy = 1;
  UINT hw = 0, hh = 0;
  if (hi && has_depth && Hi3dViews(&hi_rtv, &hi_dsv, &hsx, &hsy, &hw, &hh)) {
    for (const RECT& r : list) {
      bool full = r.left <= 0 && r.top <= 0 && r.right >= static_cast<LONG>(rt->width) &&
                  r.bottom >= static_cast<LONG>(rt->height);
      if (full) {
        if (clear_color) b.ctx->ClearRenderTargetView(hi_rtv, rgba);
        if (clear_depth) b.ctx->ClearDepthStencilView(hi_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, z, 0);
      } else {
        ClearConstants cc{};
        cc.rect[0] = 2.0f * r.left / rt->width - 1.0f;
        cc.rect[1] = 1.0f - 2.0f * r.top / rt->height;
        cc.rect[2] = 2.0f * r.right / rt->width - 1.0f;
        cc.rect[3] = 1.0f - 2.0f * r.bottom / rt->height;
        std::memcpy(cc.color, rgba, sizeof(rgba));
        cc.depth[0] = z;
        b.ctx->UpdateSubresource(g_pipe.clear_cb, 0, nullptr, &cc, 0, 0);
        b.ctx->OMSetRenderTargets(1, &hi_rtv, clear_depth ? hi_dsv : nullptr);
        D3D11_VIEWPORT v{0, 0, static_cast<float>(hw), static_cast<float>(hh), 0, 1};
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
        ContextTouched();
      }
      if (clear_color) Hi3dDrawn(r, true);
    }
  }
  if (clear_color) {
    if (cur_msaa_) MsaaMarkDrawn();
    rt_->MarkGpuWritten();
  }
  return D3D_OK;
}

// MenuHiRes3D: before drawing into the render target, decide whether the
// draw also goes to the window-sized copy (3D into the 640x480 menus); if
// not, what was drawn there so far is put into the overlay first, so this
// draw lands on top of it.
bool Device::HiResPrepare() {
  if (!rt_ || IsD3D9() || !GetConfig().menu_hires_3d || !Ready()) return false;
  bool hi = false;
  if (Surface* z = rt_->ZBuffer()) {
    if (z->Width() == rt_->Width() && z->Height() == rt_->Height())
      hi = Hi3dPrepare(*rt_->GpuForDraw(), rt_->Width(), rt_->Height());
  }
  if (!hi) Hi3dRelease(rt_->GpuKey());
  return hi;
}

HRESULT Device::SetTransform(DWORD type, D3DMATRIX* m) {
  if (!m) return DDERR_INVALIDPARAMS;
  if (batch_.draws) {
    // The smoothing decides the matrix actually used: work it out, and if it
    // differs, send the waiting draws with the old one first.
    const D3DMATRIX old_w = world_, old_v = view_, old_p = proj_;
    const HRESULT hr = SetTransformNow(type, m);
    if (std::memcmp(&old_w, &world_, sizeof(old_w)) || std::memcmp(&old_v, &view_, sizeof(old_v)) ||
        std::memcmp(&old_p, &proj_, sizeof(old_p))) {
      const D3DMATRIX new_w = world_, new_v = view_, new_p = proj_;
      world_ = old_w, view_ = old_v, proj_ = old_p;
      wvp_dirty_ = true;
      FlushBatch(kBreakTransform);
      world_ = new_w, view_ = new_v, proj_ = new_p;
      wvp_dirty_ = true;
    }
    return hr;
  }
  return SetTransformNow(type, m);
}

HRESULT Device::SetTransformNow(DWORD type, D3DMATRIX* m) {
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
      // Undo the smoothing cull margin first, so a zoom between steps is
      // blended between drawn views (the margin starting at the gate drop is
      // not a zoom).
      const float r = SmoothProjectionScale(&m->m[0][0]);
      proj_.m[0][0] *= r;
      proj_.m[1][1] *= r;
      SmoothProjection(&proj_.m[0][0]);  // zoom between steps
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
  if (batch_.draws && std::memcmp(vp, &viewport_, sizeof(viewport_)) != 0) FlushBatch(kBreakViewport);
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
  if (batch_.draws && std::memcmp(m, &material_, sizeof(material_)) != 0) FlushBatch(kBreakLighting);
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
  FlushBatch(kBreakLighting);
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
  if (batch_.draws && rs_[state] != value) {
    ++g_bs.rs_breaks[state];
    FlushBatch(kBreakRenderState);
  }
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
  FlushBatch(kBreakSurface);
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
  d.game_verts = d.verts;
  if (ProbeStage())
    ProbeDraw(0, prim, fvf, d.layout.tl, static_cast<const uint8_t*>(v), d.layout.stride, n, nullptr, 0,
              &world_.m[0][0], &view_.m[0][0], &proj_.m[0][0], Wvp(), static_cast<float>(viewport_.dwX),
              static_cast<float>(viewport_.dwY), static_cast<float>(viewport_.dwWidth),
              static_cast<float>(viewport_.dwHeight), rs_[RS_ZENABLE] != 0);
  SmoothNoteDraw("draw", d.layout.tl, n);
  if (!d.layout.tl) {
    PROF_ZONE(kSmoothVerts);
    d.verts = SmoothVertices(d.verts, d.layout.stride, n, nullptr);
  }
  Submit(d);
  return D3D_OK;
}

namespace {
// The game's draw call this came from, when it came through the game's D3D
// wrapper (0x4ca270, which returns to us at 0x4ca2ed): that wrapper pushes
// four registers and passes on its seven arguments, so its own return address
// is 0x34 bytes above ours. `ret` is where our return address is.
uint32_t GameDrawSite(void* const* ret) {
  if (reinterpret_cast<uintptr_t>(ret[0]) != 0x4CA2ED) return 0;
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ret[0x34 / 4]));
}
// StillScenery: terrain patches (0x481c00: detail layer, ground; 0x481f20),
// and close-up trees (0x457dc0): positions placed in the world once.
bool IsSceneryDraw(uint32_t site) {
  return site == 0x481D1E || site == 0x481DFE || site == 0x4820AF || site == 0x457E14;
}
}  // namespace

HRESULT Device::DrawIndexedPrimitive(DWORD prim, DWORD fvf, void* v, DWORD nv, WORD* idx,
                                     DWORD ni, DWORD) {
#ifdef _MSC_VER
  void* const* ret_slot = static_cast<void* const*>(_AddressOfReturnAddress());
#else
  void* const* ret_slot = static_cast<void* const*>(__builtin_frame_address(0)) + 1;
#endif
  DD_LOCK;
  if (!v || !nv || !idx || !ni) return D3D_OK;
  const uint32_t site = GameDrawSite(ret_slot);
  const bool scenery = GetConfig().still_scenery && IsSceneryDraw(site);
  // FarTrees: the game's own tree billboards (FarTreeLog compares them with
  // the numbers far trees are drawn from).
  if (site == 0x45CB34 && fvf == 0x1E2) FarTreesCheckDraw(v, nv, idx, ni);
  Draw d;
  d.prim = prim;
  d.verts = static_cast<const uint8_t*>(v);
  d.layout = FvfLayout::From(fvf);
  d.count = nv;
  d.idx = idx;
  d.nidx = ni;
  d.game_verts = d.verts;
  if (ProbeStage())
    ProbeDraw(site, prim, fvf, d.layout.tl, static_cast<const uint8_t*>(v), d.layout.stride, nv, idx, ni,
              &world_.m[0][0], &view_.m[0][0], &proj_.m[0][0], Wvp(), static_cast<float>(viewport_.dwX),
              static_cast<float>(viewport_.dwY), static_cast<float>(viewport_.dwWidth),
              static_cast<float>(viewport_.dwHeight), rs_[RS_ZENABLE] != 0);
  SmoothNoteDraw("drawidx", d.layout.tl, d.count);
  if (!d.layout.tl) {
    PROF_ZONE(kSmoothVerts);
    if (scenery) SmoothNoteScenery(d.verts, d.layout.stride, nv);
    else d.verts = SmoothVertices(d.verts, d.layout.stride, nv, idx);
  }
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
  d.game_verts = d.verts;
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
  d.game_verts = d.verts;
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
  FlushBatch(kBreakTexture);
  if (s) s->Hold();
  if (tex_[stage]) tex_[stage]->Unhold();
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
  if (batch_.draws && (tss_[stage][type] != value ||
                       (type == TSS_ADDRESS && (tss_[stage][TSS_ADDRESSU] != value || tss_[stage][TSS_ADDRESSV] != value)))) {
    if (StageStateUnused(stage, type)) {
      NoteStageStateKept(stage, type);
    } else {
      // Which change split the batch, with its old and new value (BatchLog / Profile).
      const DWORD old = tss_[stage][type];
      ++g_bs.tss_breaks[(stage * 32 + type) | (std::min<DWORD>(old, 255) << 8) | (std::min<DWORD>(value, 255) << 16)];
      FlushBatch(kBreakStageState);
    }
  }
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
  FlushBatch(kBreakSurface);
  Surface* d = Surface::From(dst);
  Surface* s = Surface::From(src);
  if (!d || !s) return DDERR_INVALIDPARAMS;
  d->CopyFrom(s, dp, sr);
  return D3D_OK;
}

HRESULT Device::LightEnable(DWORD i, BOOL on) {
  FlushBatch(kBreakLighting);
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

UINT Device::PushVertices(UINT bytes, void (*fill)(void* ctx, void* dst), void* ctx) {
  Backend& b = B();
  D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (vbuf_pos_ + bytes > vbuf_size_) {
    vbuf_pos_ = 0;
    map = D3D11_MAP_WRITE_DISCARD;
    ++vgen_;
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(b.ctx->Map(vbuf_, 0, map, 0, &m))) return UINT(-1);
  // Write-only memory (often write-combined): fill only stores into it.
  fill(ctx, static_cast<uint8_t*>(m.pData) + vbuf_pos_);
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
    ++igen_;
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
  // 3D scenes (targets with a depth buffer) are drawn multisampled.
  MsaaViews mv;
  cur_msaa_ = has_depth && MsaaAcquire(rt, mv, full_clear);
  if (cur_msaa_) {
    cur_rtv_ = mv.rtv;
    cur_dsv_ = mv.dsv;
  } else {
    MsaaRelease(rt->Native());
    cur_rtv_ = rt->rtv;
    cur_dsv_ = dsv;
  }
  Bound& bd = Cache(this);
  if (bd.rtv != cur_rtv_ || bd.dsv != cur_dsv_) {
    // Never sample from the texture being drawn into.
    ID3D11ShaderResourceView* none[2] = {};
    B().ctx->PSSetShaderResources(0, 2, none);
    B().ctx->OMSetRenderTargets(1, &cur_rtv_, cur_dsv_);
    bd.srv[0] = bd.srv[1] = nullptr;
    bd.rtv = cur_rtv_;
    bd.dsv = cur_dsv_;
    ++g_cstat.targets;
  }
  prof::NoteTarget(cur_rtv_);
  return true;
}

ID3D11BlendState* Device::GetBlend(bool a2c) {
  // Alpha-to-coverage draws are not blended: their alpha becomes the MSAA
  // coverage instead (blending as well would fade the edges twice).
  bool on = rs_[RS_ALPHABLENDENABLE] != 0 && !a2c;
  DWORD src = rs_[RS_SRCBLEND], dst = rs_[RS_DESTBLEND];
  if (src == 12) {  // D3DBLEND_BOTHSRCALPHA
    src = 5;
    dst = 6;
  } else if (src == 13) {  // D3DBLEND_BOTHINVSRCALPHA
    src = 6;
    dst = 5;
  }
  uint32_t key = (on ? (1u | (src << 1) | (dst << 8)) : 0) | (a2c ? 0x10000u : 0);
  auto it = blend_cache_.find(key);
  if (it != blend_cache_.end()) return it->second;
  D3D11_BLEND_DESC bd{};
  bd.AlphaToCoverageEnable = a2c;
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
  // Anisotropy is in the key: it can change from the Options menu.
  const uint32_t aniso = static_cast<uint32_t>(GetConfig().anisotropy) & 0x1f;
  uint32_t key = mag | (min << 3) | (mip << 6) | (au << 8) | (av << 11) | (maxlevel << 14) |
                 (static_cast<uint32_t>(bias) << 18) | (aniso << 26);
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

Device::Cutout Device::CutoutDraw() const {
  if (!rs_[RS_ZWRITEENABLE] || !rs_[RS_ZENABLE] || !rs_[RS_ALPHATESTENABLE] ||
      !(rs_[RS_ALPHAFUNC] == 5 || rs_[RS_ALPHAFUNC] == 7))  // GREATER, GREATEREQUAL
    return kNoCutout;
  if (!rs_[RS_ALPHABLENDENABLE]) {
    // Solid models with see-through parts: the game tests alpha against 128
    // (192 for some plants). Reference 0 is the terrain, whose alpha is not
    // a cut-out.
    return (rs_[RS_ALPHAREF] & 0xff) > 0 && tex_[0] ? kTestedCutout : kNoCutout;
  }
  // Blended cut-outs (leaves, flags) that also write depth hide whatever is
  // drawn behind them later, even where they are nearly transparent; the
  // original game shows a stair-step fringe there. The game marks them with
  // alpha test GREATER 0, which lets every filtered edge pixel through; soft
  // layers (terrain detail, shadows) have alpha test off and are left alone.
  if (!(rs_[RS_SRCBLEND] == 5 || rs_[RS_SRCBLEND] == 12)) return kNoCutout;  // SRCALPHA, BOTHSRCALPHA
  // ...except soft layers that are drawn the same way (supercross ground
  // noise: mostly half-transparent texels).
  if (tex_[0] && tex_[0]->SoftAlpha()) return kNoCutout;
  return kBlendedCutout;
}

namespace {
// Alpha-to-coverage use, logged every 10 s while AlphaLog=1.
struct A2cStats {
  DWORD since = 0;
  unsigned a2c = 0, a2c_tested = 0, no_msaa = 0, cut = 0;
} g_a2c;
}  // namespace

bool Device::UseAlphaToCoverage(Cutout cutout) {
  if (cutout == kNoCutout) return false;
  const Config& cfg = GetConfig();
  const bool use = cfg.alpha_to_coverage != 0 && cur_msaa_ && MsaaSamples() > 1;
  if (use) {
    LogOnce("alpha-to-coverage on for cut-outs (%s, %u samples)",
            cfg.alpha_to_coverage == 1 ? "sharpened" : "soft for blended ones", MsaaSamples());
  } else if (cfg.alpha_to_coverage == 0) {
    LogOnce("alpha-to-coverage off (AlphaToCoverage=0): blended cut-outs use AlphaCut %d", cfg.alpha_cut);
  } else {
    LogOnce("alpha-to-coverage: cut-out drawn without MSAA (Antialiasing %d, samples %u, target %s): alpha test / AlphaCut %d used",
            cfg.msaa, MsaaSamples(), cur_msaa_ ? "multisampled" : "single-sampled", cfg.alpha_cut);
  }
  if (cfg.alpha_log) {
    ++(use ? (cutout == kTestedCutout ? g_a2c.a2c_tested : g_a2c.a2c)
           : cur_msaa_ || cfg.alpha_to_coverage == 0 ? g_a2c.cut : g_a2c.no_msaa);
    const DWORD now = GetTickCount();
    if (!g_a2c.since) g_a2c.since = now;
    if (now - g_a2c.since >= 10000) {
      Log("alpha-to-coverage last %.1fs: cut-out draws with coverage: %u blended, %u alpha-tested; without: %u "
          "(A2C off), %u (no MSAA target)",
          (now - g_a2c.since) / 1000.0, g_a2c.a2c, g_a2c.a2c_tested, g_a2c.cut, g_a2c.no_msaa);
      g_a2c = A2cStats{};
      g_a2c.since = now;
    }
  }
  return use;
}

void Device::ApplyState(bool tl, bool depth_clip, bool has_depth, bool shadow) {
  Backend& b = B();
  Bound& bd = Cache(this);
  const Cutout cutout = CutoutDraw();
  const bool a2c = UseAlphaToCoverage(cutout);
  ID3D11BlendState* blend = GetBlend(a2c);
  ID3D11DepthStencilState* depth = GetDepth(has_depth);
  ID3D11RasterizerState* raster = GetRaster(depth_clip);
  if (bd.blend != blend) b.ctx->OMSetBlendState(bd.blend = blend, nullptr, 0xffffffff), ++g_cstat.states;
  if (bd.depth != depth) b.ctx->OMSetDepthStencilState(bd.depth = depth, 0), ++g_cstat.states;
  if (bd.raster != raster) b.ctx->RSSetState(bd.raster = raster), ++g_cstat.states;

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
        srv[s] = tex_[s]->SampleTex()->srv;
        pc.scol[s][3] = srv[s] ? 1 : 0;
      }
    }
    smp[s] = GetSampler(s);
  }
  smp[2] = g_pipe.key_sampler;
  if (prof::On()) prof::NoteDrawState(srv[0], blend, depth, raster);
  if (bd.srv[0] != srv[0] || bd.srv[1] != srv[1]) {
    b.ctx->PSSetShaderResources(0, 2, srv);
    bd.srv[0] = srv[0];
    bd.srv[1] = srv[1];
    ++g_cstat.textures;
  }
  if (std::memcmp(bd.smp, smp, sizeof(smp)) != 0) {
    b.ctx->PSSetSamplers(0, 3, smp);
    std::memcpy(bd.smp, smp, sizeof(smp));
    ++g_cstat.textures;
  }

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
  // Cut-outs (see CutoutDraw): drop pixels below AlphaCut, or with
  // alpha-to-coverage turn alpha into MSAA coverage, sharpened at AlphaCut
  // (half way when AlphaCut is 0) or as is.
  // Alpha-tested models are always sharpened at the game's own reference
  // (as is would make their solid parts partly see-through), and their alpha
  // test is dropped so the edge below the reference stays smooth.
  if (a2c) {
    const int ac = GetConfig().alpha_cut;
    if (cutout == kTestedCutout) {
      pc.cut[0] = static_cast<float>(rs_[RS_ALPHAREF] & 0xff) / 255.0f;
      pc.cut[1] = 1;
    } else {
      pc.cut[0] = (ac > 0 ? std::min(ac, 255) : 128) / 255.0f;
      pc.cut[1] = static_cast<float>(GetConfig().alpha_to_coverage);
    }
    pc.atest[1] = 8.0f;  // coverage does the cutting
  } else if (cutout == kBlendedCutout) {
    pc.cut[0] = static_cast<float>(GetConfig().alpha_cut) / 255.0f;
  }
  // Which blended / alpha-tested / two-texture draws get the AlphaCut, once
  // per combination (ini AlphaLog=1): to tell a cut-out from a soft layer
  // such as the terrain detail texture when one goes wrong.
  if (GetConfig().alpha_log && (rs_[RS_ALPHABLENDENABLE] || rs_[RS_ALPHATESTENABLE] || srv[1])) {
    static std::set<std::string> seen;
    if (seen.size() < 400) {
      char key[400];
      auto name = [](Surface* t) -> std::string {
        if (!t) return "-";
        char b[64];
        std::snprintf(b, sizeof(b), "%lux%lu", static_cast<unsigned long>(t->Width()), static_cast<unsigned long>(t->Height()));
        return t->TextureName().empty() ? std::string(b) : t->TextureName() + " " + b;
      };
      std::snprintf(key, sizeof(key),
                    "tex0 %s | tex1 %s | blend %lu src %lu dst %lu | zwrite %lu zenable %lu | atest %lu func %lu ref "
                    "%lu | op0 %lu/%lu op1 %lu/%lu | cut %s | a2c %s",
                    name(tex_[0]).c_str(), name(srv[1] ? tex_[1] : nullptr).c_str(), rs_[RS_ALPHABLENDENABLE],
                    rs_[RS_SRCBLEND], rs_[RS_DESTBLEND], rs_[RS_ZWRITEENABLE], rs_[RS_ZENABLE],
                    rs_[RS_ALPHATESTENABLE], rs_[RS_ALPHAFUNC], rs_[RS_ALPHAREF] & 0xff, tss_[0][TSS_COLOROP],
                    tss_[0][TSS_ALPHAOP], tss_[1][TSS_COLOROP], tss_[1][TSS_ALPHAOP], pc.cut[0] > 0 && !a2c ? "yes" : "no",
                    a2c ? "yes" : "no");
      if (seen.insert(key).second) Log("alpha draw: %s", key);
    }
  }
  pc.cut[2] = detail_fade_[0];
  pc.cut[3] = detail_fade_[1];
  std::memcpy(pc.tfar, terr_far_, sizeof(pc.tfar));
  std::memcpy(pc.tfard, terr_fard_, sizeof(pc.tfard));
  std::memcpy(pc.tfars, terr_fars_, sizeof(pc.tfars));
  if (!bd.pc_ok || std::memcmp(&bd.pc, &pc, sizeof(pc)) != 0) {
    b.ctx->UpdateSubresource(ps_cb_, 0, nullptr, &pc, 0, 0);
    bd.pc = pc;
    bd.pc_ok = true;
    ++g_cstat.ps_consts;
  }

  bool flat = rs_[RS_SHADEMODE] == 1;
  if (!bd.pipe) {
    b.ctx->IASetInputLayout(g_pipe.layout);
    b.ctx->VSSetConstantBuffers(0, 1, &vs_cb_);
    b.ctx->PSSetConstantBuffers(1, 1, &ps_cb_);
    const UINT stride = sizeof(GpuVertex), zero = 0;
    b.ctx->IASetVertexBuffers(0, 1, &vbuf_, &stride, &zero);
    b.ctx->IASetIndexBuffer(ibuf_, DXGI_FORMAT_R16_UINT, 0);
    bd.pipe = true;
  }
  ID3D11VertexShader* vs = shadow ? g_pipe.vs_shadow[flat] : g_pipe.vs[flat];
  ID3D11PixelShader* ps = shadow ? g_pipe.ps_shadow[flat] : g_pipe.ps[flat];
  if (bd.vs != vs) b.ctx->VSSetShader(bd.vs = vs, nullptr, 0);
  if (bd.ps != ps) b.ctx->PSSetShader(bd.ps = ps, nullptr, 0);
  (void)tl;
}

// Text the game drew with GDI earlier lies under this draw: erase it where the
// draw lands (screen-space vertices: their bounding box; 3D: the viewport).
void Device::OccludeText(const Draw& d) {
  if (!rt_) return;
  const void* n = rt_->GpuKey();
  if (!GpuTextHasText(n)) return;
  RECT r{static_cast<LONG>(viewport_.dwX), static_cast<LONG>(viewport_.dwY),
         static_cast<LONG>(viewport_.dwX + viewport_.dwWidth), static_cast<LONG>(viewport_.dwY + viewport_.dwHeight)};
  if (d.layout.tl && d.verts && d.layout.stride && d.count) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (DWORD i = 0; i < d.count; ++i) {
      const float* v = reinterpret_cast<const float*>(d.verts + static_cast<size_t>(d.first + i) * d.layout.stride);
      x0 = std::min(x0, v[0]);
      y0 = std::min(y0, v[1]);
      x1 = std::max(x1, v[0]);
      y1 = std::max(y1, v[1]);
    }
    RECT b{static_cast<LONG>(std::floor(x0)), static_cast<LONG>(std::floor(y0)), static_cast<LONG>(std::ceil(x1)),
           static_cast<LONG>(std::ceil(y1))};
    if (!IntersectRect(&r, &r, &b)) return;
    GpuTextOcclude(n, &r, "screen-space draw");
    return;
  }
  GpuTextOcclude(n, &r, "3D draw");
}

// Screen-space lines (the speedometer needle) are 1 pixel wide in D3D;
// ini LineWidth draws them as wider antialiased strips instead: a solid core
// and a 1 pixel edge on each side that fades out, alpha blended.
bool Device::SubmitWideLine(const Draw& d) {
  const float width = GetConfig().line_width;
  if (!(width > 1.0f) || d.idx || d.vb || !d.verts || d.count < 2) return false;
  if (d.prim != D3DPT_LINELIST && d.prim != D3DPT_LINESTRIP) return false;
  const FvfLayout& l = d.layout;
  if (l.diffuse < 0 || l.stride < 16) return false;
  const size_t stride = l.stride;
  const DWORD segs = d.prim == D3DPT_LINELIST ? d.count / 2 : d.count - 1;
  // Per segment: 3 strips (edge, core, edge) of 2 triangles.
  line_verts_.resize(static_cast<size_t>(segs) * 18 * stride);
  const float core = std::max(0.0f, width * 0.5f - 0.5f), outer = core + 1.0f;
  size_t n = 0;
  for (DWORD sgi = 0; sgi < segs; ++sgi) {
    const DWORD i0 = d.first + (d.prim == D3DPT_LINELIST ? sgi * 2 : sgi);
    const uint8_t* a = d.verts + i0 * stride;
    const uint8_t* b = a + stride;
    const float* pa = reinterpret_cast<const float*>(a);
    const float* pb = reinterpret_cast<const float*>(b);
    float dx = pb[0] - pa[0], dy = pb[1] - pa[1];
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 1e-4f)) continue;
    dx /= len, dy /= len;
    const float nx = -dy, ny = dx;
    // Ends are pushed out by half a pixel so the strip covers the line's length.
    auto put = [&](const uint8_t* src, float off, float along, DWORD alpha) {
      uint8_t* o = line_verts_.data() + n++ * stride;
      std::memcpy(o, src, stride);
      float* p = reinterpret_cast<float*>(o);
      p[0] += nx * off + dx * along;
      p[1] += ny * off + dy * along;
      DWORD c;
      std::memcpy(&c, o + l.diffuse, 4);
      c = (c & 0x00ffffff) | (alpha << 24);
      std::memcpy(o + l.diffuse, &c, 4);
    };
    const float offs[4] = {-outer, -core, core, outer};
    const DWORD alph[4] = {0, 255, 255, 0};
    for (int k = 0; k < 3; ++k) {
      put(a, offs[k], -0.5f, alph[k]);
      put(b, offs[k], 0.5f, alph[k]);
      put(b, offs[k + 1], 0.5f, alph[k + 1]);
      put(a, offs[k], -0.5f, alph[k]);
      put(b, offs[k + 1], 0.5f, alph[k + 1]);
      put(a, offs[k + 1], -0.5f, alph[k + 1]);
    }
  }
  if (!n) return true;
  static unsigned logged = 0;
  if (logged < 8) {
    ++logged;
    const float* p = reinterpret_cast<const float*>(d.verts + d.first * stride);
    DWORD c;
    std::memcpy(&c, d.verts + d.first * stride + l.diffuse, 4);
    Log("wide line: %lu segment(s) %s from (%.1f %.1f) colour %08lx, width %.1f, blend %lu src %lu dst %lu", segs,
        d.prim == D3DPT_LINELIST ? "list" : "strip", p[0], p[1], c, width, rs_[RS_ALPHABLENDENABLE],
        rs_[RS_SRCBLEND], rs_[RS_DESTBLEND]);
  }
  // Untextured, vertex colour and alpha, blended; put the game's state back after.
  const DWORD rs_keep[5] = {rs_[RS_ALPHABLENDENABLE], rs_[RS_SRCBLEND], rs_[RS_DESTBLEND], rs_[RS_ALPHATESTENABLE],
                            rs_[RS_FOGENABLE]};
  DWORD tss_keep[2][7];
  for (int st = 0; st < 2; ++st)
    for (int k = 0; k < 7; ++k) tss_keep[st][k] = tss_[st][TSS_COLOROP + k];
  Surface* tex_keep[2] = {tex_[0], tex_[1]};
  rs_[RS_ALPHABLENDENABLE] = 1;
  rs_[RS_SRCBLEND] = 5;   // SRCALPHA
  rs_[RS_DESTBLEND] = 6;  // INVSRCALPHA
  rs_[RS_ALPHATESTENABLE] = 0;
  rs_[RS_FOGENABLE] = 0;
  tss_[0][TSS_COLOROP] = 2;  // SELECTARG1
  tss_[0][TSS_COLORARG1] = 0;  // DIFFUSE
  tss_[0][TSS_ALPHAOP] = 2;
  tss_[0][TSS_ALPHAARG1] = 0;
  tss_[1][TSS_COLOROP] = D3DTOP_DISABLE;
  tss_[1][TSS_ALPHAOP] = D3DTOP_DISABLE;
  tex_[0] = tex_[1] = nullptr;
  Draw w = d;
  w.prim = D3DPT_TRIANGLELIST;
  w.verts = line_verts_.data();
  w.first = 0;
  w.count = static_cast<DWORD>(n);
  SubmitDraw(w);
  rs_[RS_ALPHABLENDENABLE] = rs_keep[0];
  rs_[RS_SRCBLEND] = rs_keep[1];
  rs_[RS_DESTBLEND] = rs_keep[2];
  rs_[RS_ALPHATESTENABLE] = rs_keep[3];
  rs_[RS_FOGENABLE] = rs_keep[4];
  for (int st = 0; st < 2; ++st)
    for (int k = 0; k < 7; ++k) tss_[st][TSS_COLOROP + k] = tss_keep[st][k];
  tex_[0] = tex_keep[0];
  tex_[1] = tex_keep[1];
  return true;
}

void Device::Submit(const Draw& d) {
  PROF_ZONE(kSubmit);
  prof::Count(prof::cDraws);
  if (d.layout.tl) prof::Count(prof::cDrawsTL);
  prof::Count(prof::cVerts, d.count);
  if (d.idx) {
    prof::Count(prof::cDrawsIndexed);
    prof::Count(prof::cIndices, d.nidx);
  }
  if (DrawLogOn()) NoteDrawLog(d);
  if (terr_probe_left_ > 0 && !d.layout.tl) LogTerrainProbe(d);
  if (!d.layout.tl) {
    if (MotionBlurOn() && MotionBlurNote(d, false)) return;
    // Reflections: bike and rider draws go unbatched, reflective ones with
    // their reflection drawn on top (reflect.cpp).
    if (ReflectMode() && !IsD3D9()) {
      const bool reflect = ReflectWants(d);
      if (reflect || (ReflectRecording() && ReflectCurrentObject())) {
        FlushBatch(kBreakShape);
        submitting_object_ = true;
        SubmitDraw(d);
        submitting_object_ = false;
        if (reflect) ReflectPass(d);
        return;
      }
    }
    if (BatchAdd(d)) return;
    return SubmitDraw(d);
  }
  FlushBatch(kBreakScreen);
  if (MotionBlurOn()) MotionBlurNote(d, true);
  if (SubmitWideLine(d)) return;
  // Race HUD text (hudtext.cpp): the name labels over the riders are drawn
  // larger, about the middle of their bottom edge (which sits over the head).
  const int uv = d.layout.ntex > 0 ? d.layout.tex[0] : -1;
  const float k = HudTextLabelScale(tex_[0], d.verts, d.first, d.count, d.layout.stride);
  if (k != 1.0f && d.verts && d.count && !d.vb) {
    const size_t stride = d.layout.stride;
    label_verts_.assign(d.verts, d.verts + (static_cast<size_t>(d.first) + d.count) * stride);
    float x0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (DWORD i = d.first; i < d.first + d.count; ++i) {
      const float* p = reinterpret_cast<const float*>(label_verts_.data() + i * stride);
      x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
    }
    const float cx = (x0 + x1) * 0.5f;
    for (DWORD i = d.first; i < d.first + d.count; ++i) {
      float* p = reinterpret_cast<float*>(label_verts_.data() + i * stride);
      p[0] = cx + (p[0] - cx) * k;
      p[1] = y1 + (p[1] - y1) * k;
    }
    Draw scaled = d;
    scaled.verts = label_verts_.data();
    SubmitDraw(scaled);
    HudTextAfterDraw(rt_, tex_[0], scaled.verts, d.first, d.count, d.layout.stride, uv);
    return;
  }
  SubmitDraw(d);
  // A race HUD text panel: its text goes over it.
  HudTextAfterDraw(rt_, tex_[0], d.verts, d.first, d.count, d.layout.stride, uv);
}


namespace {
uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
}  // namespace

void Device::NoteDrawLog(const Draw& d) {
  DrawLogRec r;
  r.api = d.vb ? 3 : (d.idx ? 1 : 0);
  r.prim = static_cast<uint8_t>(d.prim);
  r.tl = d.layout.tl;
  r.clip = d.layout.tl && d.vb && d.vb->HasClipData(d.first, d.count);
  r.stride = static_cast<uint16_t>(d.layout.stride);
  {
    const FvfLayout& l = d.layout;
    const int f[] = {static_cast<int>(l.stride), l.tl ? 1 : 0, l.diffuse, l.specular, l.normal, l.ntex,
                     l.tex[0], l.tex[1], l.tex_dim[0], l.tex_dim[1]};
    r.fvf_sig = static_cast<uint32_t>(Fnv(f, sizeof(f)));
  }
  r.count = d.count;
  r.nidx = d.idx ? d.nidx : 0;
  r.tex0 = tex_[0];
  r.tex1 = tex_[1];
  uint64_t m = Fnv(&world_, sizeof(world_));
  m = Fnv(&view_, sizeof(view_), m);
  m = Fnv(&proj_, sizeof(proj_), m);
  m = Fnv(&viewport_, sizeof(viewport_), m);
  r.matrices = static_cast<uint32_t>(m ^ (m >> 32));
  uint64_t h = Fnv(rs_, sizeof(rs_));
  h = Fnv(tss_, sizeof(tss_[0]) * kStages, h);
  h = Fnv(tex_, sizeof(tex_[0]) * kStages, h);
  h = Fnv(&material_, sizeof(material_), h);
  h = Fnv(&rt_, sizeof(rt_), h);
  h = Fnv(&m, sizeof(m), h);
  r.state = h;
  DrawLogNote(r);
}

void Device::SubmitDraw(const Draw& d) {
  // MenuHiRes3D: 3D in the 640x480 menus is drawn again at window size.
  const bool hi = HiResPrepare();
  OccludeText(d);
  if (prof::On()) GpuTimerMark();
  if (IsD3D9()) return Submit9(d);
  bool has_depth = false;
  {
    PROF_ZONE(kStateApply);
    if (!BindTarget(has_depth)) return;
  }
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

  prof::Scope prep_zone(prof::kVertexPrep);
  int set0 = static_cast<int>(tss_[0][TSS_TEXCOORDINDEX] & 0xffff);
  int set1 = static_cast<int>(tss_[1][TSS_TEXCOORDINDEX] & 0xffff);
  const VertexBuffer::Processed* clip = mode == 2 ? d.vb->ClipData() : nullptr;
  const int t0 = set0 < l.ntex ? l.tex[set0] : -1, t1 = set1 < l.ntex ? l.tex[set1] : -1;
  simd::VertexIn in;
  in.stride = l.stride;
  in.xyzw = l.tl;
  in.diffuse = l.diffuse;
  in.specular = l.specular;
  in.t0 = t0;
  in.t1 = t1;
  in.t0_uv = t0 >= 0 && l.tex_dim[set0] >= 2;
  in.t1_uv = t1 >= 0 && l.tex_dim[set1] >= 2;
  const uint8_t* src = d.verts + static_cast<size_t>(d.first) * l.stride;
  UINT vbytes = d.count * sizeof(GpuVertex);
  if (vbytes > vbuf_size_) {
    LogOnce("draw of %lu vertices too large", d.count);
    return;
  }
  // Converted straight into the mapped vertex buffer (no copy through a
  // scratch buffer); processed vertices then get their clip positions.
  struct Fill {
    const simd::VertexIn* in;
    const uint8_t* src;
    DWORD count;
    const VertexBuffer::Processed* clip;
    DWORD first;
  } fill{&in, src, d.count, clip, d.first};
  UINT voff = PushVertices(vbytes, [](void* c, void* dst) {
    const Fill& f = *static_cast<const Fill*>(c);
    simd::ConvertVertices(*f.in, f.src, f.count, dst);
    if (f.clip) {
      auto* out = static_cast<GpuVertex*>(dst);
      for (DWORD i = 0; i < f.count; ++i) std::memcpy(out[i].pos, f.clip[f.first + i].clip, 16);
    }
  }, &fill);
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
  prep_zone.Switch(prof::kStateApply);
  Bound& bd = Cache(this);
  ++g_cstat.draws;
  ReportCache();
  if (!bd.vc_ok || std::memcmp(&bd.vc, &vc, sizeof(vc)) != 0) {
    b.ctx->UpdateSubresource(vs_cb_, 0, nullptr, &vc, 0, 0);
    bd.vc = vc;
    bd.vc_ok = true;
    ++g_cstat.vs_consts;
  }
  D3D11_VIEWPORT v{vc.vp[0], vc.vp[1], vc.vp[2], vc.vp[3], 0.0f, 1.0f};
  if (mode != 1) {
    v.MinDepth = vp7.dvMinZ;
    v.MaxDepth = vp7.dvMaxZ;
  }
  if (!bd.vp_ok || std::memcmp(&bd.vp, &v, sizeof(v)) != 0) {
    b.ctx->RSSetViewports(1, &v);
    bd.vp = v;
    bd.vp_ok = true;
  }
  // Terrain patches get the rider shadows (gpushadow.cpp).
  const bool receiver = flushing_ ? batch_out_.receiver : ShadowReceiverNow();
  const bool shadow = receiver && mode == 0 && has_depth && g_pipe.shadow_ok &&
                      ShadowBindReceiver(&world_.m[0][0], rs_[RS_ALPHABLENDENABLE] != 0, rs_[RS_SRCBLEND], rs_[RS_DESTBLEND]);
  ApplyState(l.tl, mode != 1, has_depth, shadow);

  // The vertex and index buffers stay bound at offset 0 (ApplyState); each
  // draw starts at its own place in them.
  if (bd.topo != topo) b.ctx->IASetPrimitiveTopology(bd.topo = topo);
  prep_zone.Switch(prof::kDriverDraw);
  const INT base_vertex = static_cast<INT>(voff / sizeof(GpuVertex));
  if (idx) {
    b.ctx->DrawIndexed(nidx, ioff / 2, base_vertex);
  } else {
    b.ctx->Draw(d.count, static_cast<UINT>(base_vertex));
  }
  ID3D11RenderTargetView* hi_rtv = nullptr;
  ID3D11DepthStencilView* hi_dsv = nullptr;
  float hsx = 1, hsy = 1;
  UINT hw = 0, hh = 0;
  if (hi && has_depth && Hi3dViews(&hi_rtv, &hi_dsv, &hsx, &hsy, &hw, &hh)) {
    // The same draw into the window-sized copy: only the viewport is scaled
    // (vertex positions are made relative to the viewport by the shader).
    b.ctx->OMSetRenderTargets(1, &hi_rtv, hi_dsv);
    D3D11_VIEWPORT hv{v.TopLeftX * hsx, v.TopLeftY * hsy, v.Width * hsx, v.Height * hsy, v.MinDepth, v.MaxDepth};
    b.ctx->RSSetViewports(1, &hv);
    if (idx) {
      b.ctx->DrawIndexed(nidx, ioff / 2, base_vertex);
    } else {
      b.ctx->Draw(d.count, static_cast<UINT>(base_vertex));
    }
    b.ctx->OMSetRenderTargets(1, &cur_rtv_, cur_dsv_);
    b.ctx->RSSetViewports(1, &v);
    RECT vr{static_cast<LONG>(viewport_.dwX), static_cast<LONG>(viewport_.dwY),
            static_cast<LONG>(viewport_.dwX + viewport_.dwWidth), static_cast<LONG>(viewport_.dwY + viewport_.dwHeight)};
    // Screen-space draws only count inside the 3D area (they are in the copy too).
    if (mode == 1) vr = RECT{};
    Hi3dDrawn(vr, false);
  }
  if (cur_msaa_) MsaaMarkDrawn();
  if (has_depth) Note3DDraw();
  rt_->MarkGpuWritten();
  ++Counters().draws;
  // Reflections=2: keep world draws (not bikes, riders or the sky) to draw
  // again into the reflection cube.
  if (mode == 0 && has_depth && !submitting_object_ && !ReflectCurrentObject() && ReflectRecording() &&
      ReflectFrameDue() &&
      !(tex_[0] && IsSkyTextureName(tex_[0]->TextureName()))) {
    prep_zone.Switch(prof::kReflectCache);
    ReflectReplay r{};
    r.blend = bd.blend;
    r.depth = bd.depth;
    r.srv[0] = bd.srv[0];
    r.srv[1] = bd.srv[1];
    std::memcpy(r.smp, bd.smp, sizeof(r.smp));
    r.vs = bd.vs;
    r.ps = bd.ps;
    // Rider shadows: the cube gets the plain terrain shader (the shadow
    // atlas is not bound there, and is not wanted in a reflection).
    for (int f = 0; f < 2; ++f) {
      if (r.vs == g_pipe.vs_shadow[f]) r.vs = g_pipe.vs[f];
      if (r.ps == g_pipe.ps_shadow[f]) r.ps = g_pipe.ps[f];
    }
    std::memcpy(r.world, &world_, sizeof(r.world));
    static_assert(sizeof(PSConstants) <= sizeof(r.pc), "replay constants");
    std::memcpy(r.pc, &bd.pc, sizeof(PSConstants));
    r.pc_size = sizeof(PSConstants);
    r.topo = topo;
    r.indexed = idx != nullptr;
    r.nidx = nidx;
    r.ioff = ioff;
    r.count = d.count;
    r.base_vertex = base_vertex;
    r.vgen = vgen_;
    r.igen = igen_;
    // ReflectMaxRange: far scenery (long view distances) is left out of the
    // cube; its bounds come from a sample of the positions.
    bool in_range = true;
    if (d.verts && d.layout.stride) {
      if (flushing_) {
        range_starts_.clear();
        range_counts_.clear();
        for (const ReflectSub& sb : batch_out_.subs) {
          range_starts_.push_back(sb.vstart);
          range_counts_.push_back(sb.nverts);
        }
        in_range = ReflectInRange(r, batch_out_.verts.data(), d.layout.stride, range_starts_.data(),
                                  range_counts_.data(), range_starts_.size());
      } else {
        const unsigned start = 0, count = d.count;
        in_range = ReflectInRange(r, d.verts + static_cast<size_t>(d.first) * d.layout.stride, d.layout.stride,
                                  &start, &count, 1);
      }
    } else {
      r.bounded = false;
    }
    if (!in_range) return;
    ReflectRecord(r);
    // Kept for later frames (behind the camera), when the draw writes depth.
    if (rs_[RS_ZWRITEENABLE] && d.verts && d.layout.stride) {
      if (flushing_) {
        ReflectCache(r, batch_out_.verts.data(), d.layout.stride, batch_out_.subs.data(), batch_out_.subs.size(),
                     sizeof(GpuVertex));
      } else {
        const ReflectSub one{0, d.count, 0, idx ? nidx : 0};
        ReflectCache(r, d.verts + static_cast<size_t>(d.first) * d.layout.stride, d.layout.stride, &one, 1,
                     sizeof(GpuVertex));
      }
    }
  }
}

// ---- FarTrees (fartrees.cpp)

namespace {
struct TreeConstants {
  float tr0[4], tr1[4], tr2[4], tr3[4], tcol[4], tspec[4];
};
}  // namespace

bool FarTreesDrawable() {
  if (!g_tree_device || IsD3D9()) return false;
  return InitPipeline() && g_pipe.vs_tree[0] && g_pipe.vs_tree[1];
}

bool DrawFarTrees(const FarTreeFrame& f) {
  if (!g_tree_device) return false;
  return g_tree_device->DrawFarTrees(f);
}

bool Device::DrawFarTrees(const FarTreeFrame& f) {
  if (IsD3D9() || !InitPipeline() || !g_pipe.vs_tree[0] || !g_pipe.vs_tree[1]) return false;
  if (!f.count || !f.trees) return true;
  FlushBatch(kBreakScreen);
  Backend& b = B();
  // The trees' numbers: a buffer that grows; only new trees are sent.
  if (f.count > tree_cap_) {
    {
      size_t cap = tree_cap_ ? tree_cap_ : 4096;
      while (cap < f.count) cap *= 2;
      SafeRelease(tree_srv_);
      SafeRelease(tree_buf_);
      D3D11_BUFFER_DESC bd{};
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.ByteWidth = static_cast<UINT>(cap * 48);
      bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(b.device->CreateBuffer(&bd, nullptr, &tree_buf_))) {
        LogOnce("far trees: buffer of %zu trees failed", cap);
        tree_cap_ = 0;
        return false;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
      sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      sd.Buffer.FirstElement = 0;
      sd.Buffer.NumElements = static_cast<UINT>(cap * 3);
      if (FAILED(b.device->CreateShaderResourceView(tree_buf_, &sd, &tree_srv_))) {
        SafeRelease(tree_buf_);
        tree_cap_ = 0;
        return false;
      }
      tree_cap_ = cap;
    }
    tree_sent_ = 0;
  }
  if (f.dirty_from < tree_sent_) tree_sent_ = f.dirty_from;
  if (tree_sent_ < f.count) {
    D3D11_BOX box{static_cast<UINT>(tree_sent_ * 48), 0, 0, static_cast<UINT>(f.count * 48), 1, 1};
    b.ctx->UpdateSubresource(tree_buf_, 0, &box, f.trees + tree_sent_ * 12, 0, 0);
    tree_sent_ = f.count;
  }
  if (!tree_cb_) {
    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(TreeConstants);
    if (FAILED(b.device->CreateBuffer(&cb, nullptr, &tree_cb_))) return false;
  }
  bool has_depth = false;
  if (!BindTarget(has_depth)) return false;
  TreeConstants tc{};
  tc.tr0[0] = f.right[0];
  tc.tr0[1] = f.right[1];
  tc.tr0[2] = f.view[0];
  tc.tr0[3] = f.view[1];
  tc.tr1[0] = f.view[2];
  tc.tr1[1] = f.key_scale;
  tc.tr1[2] = f.key_unit;
  tc.tr1[3] = f.far_depth - f.eps;
  tc.tr2[0] = f.max_depth;
  tc.tr2[1] = f.focal;
  tc.tr2[2] = f.min_size;
  tc.tr2[3] = f.six_ac;
  tc.tr3[0] = f.six_a8 ? 1.0f : 0.0f;
  tc.tr3[1] = f.six_84 ? 1.0f : 0.0f;
  tc.tr3[2] = f.six_near;
  tc.tr3[3] = f.eps;
  ColorToFloat(f.diffuse, tc.tcol);
  ColorToFloat(f.specular, tc.tspec);
  b.ctx->UpdateSubresource(tree_cb_, 0, nullptr, &tc, 0, 0);

  VSConstants vc{};
  std::memcpy(vc.wvp, Wvp(), sizeof(vc.wvp));
  vc.vp[0] = static_cast<float>(viewport_.dwX);
  vc.vp[1] = static_cast<float>(viewport_.dwY);
  vc.vp[2] = static_cast<float>(viewport_.dwWidth ? viewport_.dwWidth : 1);
  vc.vp[3] = static_cast<float>(viewport_.dwHeight ? viewport_.dwHeight : 1);
  vc.mode[0] = 0;
  Bound& bd = Cache(this);
  if (!bd.vc_ok || std::memcmp(&bd.vc, &vc, sizeof(vc)) != 0) {
    b.ctx->UpdateSubresource(vs_cb_, 0, nullptr, &vc, 0, 0);
    bd.vc = vc;
    bd.vc_ok = true;
  }
  D3D11_VIEWPORT v{vc.vp[0], vc.vp[1], vc.vp[2], vc.vp[3], viewport_.dvMinZ, viewport_.dvMaxZ};
  if (!bd.vp_ok || std::memcmp(&bd.vp, &v, sizeof(v)) != 0) {
    b.ctx->RSSetViewports(1, &v);
    bd.vp = v;
    bd.vp_ok = true;
  }
  ApplyState(false, true, has_depth, false);
  ID3D11VertexShader* vs = g_pipe.vs_tree[rs_[RS_SHADEMODE] == 1 ? 1 : 0];
  if (bd.vs != vs) b.ctx->VSSetShader(bd.vs = vs, nullptr, 0);
  b.ctx->VSSetShaderResources(4, 1, &tree_srv_);
  b.ctx->VSSetConstantBuffers(3, 1, &tree_cb_);
  if (bd.topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
    b.ctx->IASetPrimitiveTopology(bd.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  b.ctx->Draw(static_cast<UINT>(f.count * 6), 0);
  ID3D11ShaderResourceView* none = nullptr;
  b.ctx->VSSetShaderResources(4, 1, &none);
  if (cur_msaa_) MsaaMarkDrawn();
  if (has_depth) Note3DDraw();
  rt_->MarkGpuWritten();
  ++Counters().draws;
  return true;
}

// ---- GpuTerrain (gputerrain.cpp)

bool GpuTerrainDrawable() { return g_tree_device && !IsD3D9(); }

bool DrawGpuTerrain(const gt::Mesh& m, gt::DrawStats* st) {
  if (!g_tree_device) return false;
  return g_tree_device->DrawGpuTerrain(m, st);
}

void* GpuTerrainCurrentTexture() {
  if (!g_tree_device) return nullptr;
  Surface* t = g_tree_device->Texture0();
  if (t) t->AddRef();
  return t;
}

void GpuTerrainReleaseTexture(void* t) {
  if (t) static_cast<Surface*>(t)->Release();
}

void GpuTerrainAddRefTexture(void* t) {
  if (t) static_cast<Surface*>(t)->AddRef();
}

void GpuTerrainCaptureState(int site, bool log) {
  if (g_tree_device) g_tree_device->CaptureTerrainState(site, log);
}

void GpuTerrainDescribeTexture(void* t, char* out, size_t n) {
  if (!t) {
    std::snprintf(out, n, "no texture");
    return;
  }
  Surface* s = static_cast<Surface*>(t);
  std::snprintf(out, n, "%ux%u \"%s\"", s->Width(), s->Height(), s->TextureName().c_str());
}

namespace {
const DWORD kSampKeys[5] = {TSS_MINFILTER, TSS_MAGFILTER, TSS_MIPFILTER, TSS_MIPMAPLODBIAS, TSS_MAXMIPLEVEL};
std::string DescribeStage(const DWORD* t, Surface* tex) {
  char b[400];
  std::snprintf(b, sizeof(b),
                "tex %s %ux%u mips %u | colorop %lu (%lu,%lu) alphaop %lu (%lu,%lu) uv %lu | min %lu mag %lu mip %lu "
                "bias %.2f maxlevel %lu aniso %lu addr %lu/%lu",
                tex ? ("\"" + tex->TextureName() + "\"").c_str() : "none", tex ? tex->Width() : 0,
                tex ? tex->Height() : 0, tex ? tex->MipLevels() : 0, t[TSS_COLOROP], t[TSS_COLORARG1],
                t[TSS_COLORARG2], t[TSS_ALPHAOP], t[TSS_ALPHAARG1], t[TSS_ALPHAARG2], t[TSS_TEXCOORDINDEX],
                t[TSS_MINFILTER], t[TSS_MAGFILTER], t[TSS_MIPFILTER], AsFloat(t[TSS_MIPMAPLODBIAS]), t[TSS_MAXMIPLEVEL],
                t[TSS_MAXANISOTROPY], t[TSS_ADDRESSU], t[TSS_ADDRESSV]);
  return b;
}
const char* kSiteName[3] = {"base pass", "one-pass (base + detail)", "detail pass"};
}  // namespace

void Device::CaptureTerrainState(int site, bool log) {
  // Which stage holds what at this site.
  const int base_stage = site == 2 ? -1 : 0;
  const int detail_stage = site == 1 ? 1 : site == 2 ? 0 : -1;
  if (base_stage >= 0) {
    for (int k = 0; k < 5; ++k) terr_samp_[0][k] = tss_[base_stage][kSampKeys[k]];
    terr_samp_ok_[0] = true;
  }
  if (detail_stage >= 0) {
    for (int k = 0; k < 5; ++k) terr_samp_[1][k] = tss_[detail_stage][kSampKeys[k]];
    terr_samp_ok_[1] = true;
  }
  if (!log) return;
  Log("gpu terrain: game's %s state: stage0 %s", kSiteName[site], DescribeStage(tss_[0], tex_[0]).c_str());
  Log("gpu terrain: game's %s state: stage1 %s", kSiteName[site], DescribeStage(tss_[1], tex_[1]).c_str());
  Log("gpu terrain: game's %s state: blend %lu src %lu dst %lu specular %lu fog %lu zfunc %lu zwrite %lu "
             "tfactor %08lx; Anisotropy %d MipBias %.2f",
             kSiteName[site], rs_[RS_ALPHABLENDENABLE], rs_[RS_SRCBLEND], rs_[RS_DESTBLEND], rs_[RS_SPECULARENABLE],
             rs_[RS_FOGENABLE], rs_[RS_ZFUNC], rs_[RS_ZWRITEENABLE], rs_[RS_TEXTUREFACTOR], GetConfig().anisotropy,
             GetConfig().mip_bias);
  terr_probe_site_ = site;
  terr_probe_left_ = 4;
}

void Device::LogTerrainProbe(const Draw& d) {
  --terr_probe_left_;
  uint32_t dif = 0, spec = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  const uint8_t* v = d.verts ? d.verts + static_cast<size_t>(d.first) * d.layout.stride : nullptr;
  if (v && d.layout.diffuse >= 0) std::memcpy(&dif, v + d.layout.diffuse, 4);
  if (v && d.layout.specular >= 0) std::memcpy(&spec, v + d.layout.specular, 4);
  if (v && d.layout.ntex > 0) std::memcpy(&u0, v + d.layout.tex[0], 4), std::memcpy(&v0, v + d.layout.tex[0] + 4, 4);
  if (v && d.layout.ntex > 1) std::memcpy(&u1, v + d.layout.tex[1], 4), std::memcpy(&v1, v + d.layout.tex[1] + 4, 4);
  Log("gpu terrain: game's %s draw: stride %u, %lu verts, first vertex diffuse %08x specular %08x uv0 (%.3f %.3f) "
             "uv1 (%.3f %.3f); stage0 %s; stage1 colorop %lu tex %s (mip %lu)",
             kSiteName[terr_probe_site_], d.layout.stride, d.count, dif, spec, u0, v0, u1, v1,
             DescribeStage(tss_[0], tex_[0]).c_str(), tss_[1][TSS_COLOROP],
             tex_[1] ? tex_[1]->TextureName().c_str() : "none", tss_[1][TSS_MIPFILTER]);
}

namespace {
// Outside the view: all 8 corners beyond one clip plane (row vectors, D3D
// clip space 0 <= z <= w).
bool BoxOutside(const float* lo, const float* hi, const float* M) {
  float c[8][4];
  for (int i = 0; i < 8; ++i) {
    const float x = (i & 1) ? hi[0] : lo[0], y = (i & 2) ? hi[1] : lo[1], z = (i & 4) ? hi[2] : lo[2];
    for (int k = 0; k < 4; ++k) c[i][k] = x * M[k] + y * M[4 + k] + z * M[8 + k] + M[12 + k];
  }
  int out[6] = {};
  for (int i = 0; i < 8; ++i) {
    const float w = c[i][3];
    out[0] += c[i][0] < -w;
    out[1] += c[i][0] > w;
    out[2] += c[i][1] < -w;
    out[3] += c[i][1] > w;
    out[4] += c[i][2] < 0;
    out[5] += c[i][2] > w;
  }
  for (int p = 0; p < 6; ++p)
    if (out[p] == 8) return true;
  return false;
}
}  // namespace

bool Device::DrawGpuTerrain(const gt::Mesh& m, gt::DrawStats* st) {
  if (IsD3D9() || !InitPipeline()) return false;
  if (m.verts.empty() || m.idx.empty()) return false;
  Backend& b = B();
  if (terr_gen_ != m.generation || !terr_vb_ || !terr_ib_) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    SafeRelease(terr_vb_);
    SafeRelease(terr_ib_);
    D3D11_BUFFER_DESC vd{};
    vd.Usage = D3D11_USAGE_IMMUTABLE;
    vd.ByteWidth = static_cast<UINT>(m.verts.size() * sizeof(gt::Vertex));
    vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vi{m.verts.data(), 0, 0};
    D3D11_BUFFER_DESC id{};
    id.Usage = D3D11_USAGE_IMMUTABLE;
    id.ByteWidth = static_cast<UINT>(((m.idx.size() * 2) + 3) & ~size_t(3));
    id.BindFlags = D3D11_BIND_INDEX_BUFFER;
    std::vector<uint16_t> padded;
    const void* idata = m.idx.data();
    if (id.ByteWidth != m.idx.size() * 2) {
      padded = m.idx;
      padded.push_back(0);
      idata = padded.data();
    }
    D3D11_SUBRESOURCE_DATA ii{idata, 0, 0};
    HRESULT hv = b.device->CreateBuffer(&vd, &vi, &terr_vb_);
    HRESULT hi = SUCCEEDED(hv) ? b.device->CreateBuffer(&id, &ii, &terr_ib_) : hv;
    if (FAILED(hv) || FAILED(hi)) {
      LogOnce("gpu terrain: buffers of %u + %u bytes failed (%08lx)", vd.ByteWidth, id.ByteWidth,
              static_cast<unsigned long>(FAILED(hv) ? hv : hi));
      SafeRelease(terr_vb_);
      SafeRelease(terr_ib_);
      return false;
    }
    terr_gen_ = m.generation;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    if (st) {
      st->uploaded = true;
      st->upload_ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(fq.QuadPart);
    }
  }
  FlushBatch(kBreakScreen);
  // Rider shadows: drawn into their atlas first if the game's pre-draw did
  // not (it renders to its own target, so before ours is bound).
  const bool shadow_frame = g_pipe.shadow_ok && ShadowEnsureFrame();
  bool has_depth = false;
  if (!BindTarget(has_depth)) return false;

  // World space: view * projection only.
  float vpm[16];
  MatMul(&view_.m[0][0], &proj_.m[0][0], vpm);

  // The game's state for this draw, put back afterwards.
  const DWORD rs_keys[] = {RS_ZENABLE, RS_ZWRITEENABLE, RS_ALPHABLENDENABLE, RS_ALPHATESTENABLE, RS_CULLMODE,
                           RS_SHADEMODE, RS_ZFUNC};
  const DWORD rs_vals[] = {1, 1, 0, 0, 1 /* none */, 2 /* gouraud */, 4 /* less-equal */};
  constexpr int kRs = 7;
  static int logged = 0;
  if (logged < 3) {
    // Camera and state at the first draws, for the log.
    ++logged;
    const float a = proj_.m[2][2], c = proj_.m[3][2];
    const float zn = (a != 0) ? -c / a : 0, zf = (a != 1) ? c / (1 - a) : 0;
    Log("gpu terrain draw %d: proj near %.2f far %.1f (m22 %.5f m32 %.3f m23 %.2f); view row3 %.1f %.1f %.1f; "
        "viewport %lu,%lu %lux%lu z %.2f-%.2f; zenable %lu zwrite %lu zfunc %lu fog %lu (%lu/%lu) colour %08lx; "
        "depth target %d",
        logged, zn, zf, a, c, proj_.m[2][3], view_.m[3][0], view_.m[3][1], view_.m[3][2], viewport_.dwX,
        viewport_.dwY, viewport_.dwWidth, viewport_.dwHeight, viewport_.dvMinZ, viewport_.dvMaxZ, rs_[RS_ZENABLE],
        rs_[RS_ZWRITEENABLE], rs_[RS_ZFUNC], rs_[RS_FOGENABLE], rs_[RS_FOGTABLEMODE], rs_[RS_FOGVERTEXMODE],
        rs_[RS_FOGCOLOR], has_depth ? 1 : 0);
  }
  DWORD rs_save[kRs];
  for (int i = 0; i < kRs; ++i) rs_save[i] = rs_[rs_keys[i]], rs_[rs_keys[i]] = rs_vals[i];
  Surface* tex_save[2] = {tex_[0], tex_[1]};
  DWORD tss_save[2][32];
  std::memcpy(tss_save, tss_, sizeof(tss_save));
  tex_[0] = tex_[1] = nullptr;
  tss_[0][TSS_COLORARG1] = 2;  // texture
  tss_[0][TSS_COLORARG2] = 0;  // diffuse
  tss_[0][TSS_ALPHAOP] = 3;    // select arg 2: diffuse
  tss_[0][TSS_ALPHAARG2] = 0;
  tss_[0][TSS_TEXCOORDINDEX] = 0;
  tss_[0][TSS_ADDRESS] = tss_[0][TSS_ADDRESSU] = tss_[0][TSS_ADDRESSV] = 1;  // wrap
  tss_[0][TSS_MAGFILTER] = 2;  // linear
  tss_[0][TSS_MINFILTER] = 2;
  tss_[0][TSS_MIPFILTER] = 3;  // linear between mip levels
  tss_[0][TSS_MIPMAPLODBIAS] = 0;
  tss_[0][TSS_MAXMIPLEVEL] = 0;
  tss_[1][TSS_COLOROP] = 1;    // disable
  tss_[1][TSS_ALPHAOP] = 1;
  // The detail texture (as the game's one-pass mode, 0x50be69): stage 1 on
  // the second uv, its colour blended in by its own alpha.
  Surface* detail = static_cast<Surface*>(m.detail);
  tss_[1][TSS_COLORARG1] = 2;  // texture
  tss_[1][TSS_COLORARG2] = 1;  // current
  tss_[1][TSS_ALPHAARG2] = 1;
  tss_[1][TSS_TEXCOORDINDEX] = 1;
  tss_[1][TSS_ADDRESS] = tss_[1][TSS_ADDRESSU] = tss_[1][TSS_ADDRESSV] = 1;
  tss_[1][TSS_MAGFILTER] = 2;
  tss_[1][TSS_MINFILTER] = 2;
  tss_[1][TSS_MIPFILTER] = 3;
  tss_[1][TSS_MIPMAPLODBIAS] = 0;
  tss_[1][TSS_MAXMIPLEVEL] = 0;
  // Sample them as the game set its own terrain passes up (captured where
  // it would have drawn them, GpuTerrainCaptureState).
  for (int st = 0; st < 2; ++st)
    if (terr_samp_ok_[st])
      for (int k = 0; k < 5; ++k) tss_[st][kSampKeys[k]] = terr_samp_[st][k];

  VSConstants vc{};
  std::memcpy(vc.wvp, vpm, sizeof(vc.wvp));
  vc.vp[0] = static_cast<float>(viewport_.dwX);
  vc.vp[1] = static_cast<float>(viewport_.dwY);
  vc.vp[2] = static_cast<float>(viewport_.dwWidth ? viewport_.dwWidth : 1);
  vc.vp[3] = static_cast<float>(viewport_.dwHeight ? viewport_.dwHeight : 1);
  vc.mode[0] = 0;
  Bound& bd = Cache(this);
  if (!bd.vc_ok || std::memcmp(&bd.vc, &vc, sizeof(vc)) != 0) {
    b.ctx->UpdateSubresource(vs_cb_, 0, nullptr, &vc, 0, 0);
    bd.vc = vc;
    bd.vc_ok = true;
  }
  D3D11_VIEWPORT v{vc.vp[0], vc.vp[1], vc.vp[2], vc.vp[3], viewport_.dvMinZ, viewport_.dvMaxZ};
  if (!bd.vp_ok || std::memcmp(&bd.vp, &v, sizeof(v)) != 0) {
    b.ctx->RSSetViewports(1, &v);
    bd.vp = v;
    bd.vp_ok = true;
  }
  detail_fade_[0] = m.detail_fade[0];
  detail_fade_[1] = m.detail_fade[1];
  if (m.far_dist > 0 && m.focal > 0) {
    terr_far_[0] = m.far_dist;
    terr_far_[1] = m.focal;
    terr_far_[2] = vc.vp[0] + vc.vp[2] * 0.5f;
    terr_far_[3] = vc.vp[1] + vc.vp[3] * 0.5f;
    ColorToFloat(m.far_diffuse, terr_fard_);
    ColorToFloat(m.far_specular, terr_fars_);
    terr_fars_[3] = m.far_dist2;
  }
  // Visible chunks' ranges, sorted by texture so each is bound once.
  std::vector<const gt::Range*>& todo = terr_todo_;
  std::vector<int32_t>& todo_base = terr_todo_base_;
  todo.clear();
  todo_base.clear();
  unsigned drawn = 0, tris = 0, draws = 0;
  for (const gt::Chunk& c : m.chunks) {
    if (BoxOutside(c.lo, c.hi, vpm)) continue;
    ++drawn;
    tris += c.index_count / 3;
    for (uint32_t r = 0; r < c.range_count; ++r) {
      todo.push_back(&m.ranges[c.first_range + r]);
      todo_base.push_back(c.base_vertex);
    }
  }
  std::vector<uint32_t>& order = terr_order_;
  order.resize(todo.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t c) {
              return todo[a]->material != todo[c]->material ? todo[a]->material < todo[c]->material
                                                             : todo[a]->far_material < todo[c]->far_material;
            });
  {
    ID3D11SamplerState* s0 = GetSampler(0);  // the base texture's (the game's settings)
    b.ctx->PSSetSamplers(4, 1, &s0);
  }
  // Rider shadows (gpushadow.cpp): the mesh is in world space, so the
  // light-space matrix is the light's alone.
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const bool shadow = has_depth && shadow_frame && ShadowBindReceiver(kIdentity, false, 0, 0);
  if (st) st->shadowed = shadow;
  // One texture pair's state (base + far texture, detail on stage 1).
  auto bind_range = [&](const gt::Range& r, bool shadowed) {
    Surface* t = (r.material >= 0 && static_cast<size_t>(r.material) < m.textures.size())
                     ? static_cast<Surface*>(m.textures[r.material])
                     : nullptr;
    tex_[0] = t;
    tss_[0][TSS_COLOROP] = t ? 4 : 3;  // modulate texture by diffuse, or diffuse alone
    tex_[1] = (t && detail) ? detail : nullptr;
    {
      Surface* ft = (r.far_material >= 0 && static_cast<size_t>(r.far_material) < m.textures.size())
                        ? static_cast<Surface*>(m.textures[r.far_material])
                        : nullptr;
      ID3D11ShaderResourceView* fsrv = ft ? ft->SampleTex()->srv : nullptr;
      b.ctx->PSSetShaderResources(6, 1, &fsrv);
    }
    tss_[1][TSS_COLOROP] = tex_[1] ? 13 : 1;  // blend texture alpha, or off
    tss_[1][TSS_ALPHAOP] = tex_[1] ? 4 - 1 : 1;  // select arg 2 (current)
    ApplyState(false, true, has_depth, shadowed);
  };
  int32_t bound = -2, bound_far = -2;
  for (uint32_t i : order) {
    const gt::Range& r = *todo[i];
    if (r.material != bound || r.far_material != bound_far) {
      bound = r.material;
      bound_far = r.far_material;
      bind_range(r, shadow);
      const UINT stride = sizeof(gt::Vertex), zero = 0;
      b.ctx->IASetVertexBuffers(0, 1, &terr_vb_, &stride, &zero);
      b.ctx->IASetIndexBuffer(terr_ib_, DXGI_FORMAT_R16_UINT, 0);
      if (bd.topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
        b.ctx->IASetPrimitiveTopology(bd.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    b.ctx->DrawIndexed(r.index_count, r.first_index, todo_base[i]);
    ++draws;
  }
  // Reflections=2: the terrain near the bike, in view or not, drawn again
  // into the live reflection cube (the game's own terrain draws are what it
  // replays otherwise, and those are skipped while GPU terrain draws).
  if (has_depth && ReflectRecording() && ReflectFrameDue()) {
    todo.clear();
    todo_base.clear();
    std::vector<const gt::Chunk*>& todo_chunk = terr_todo_chunk_;
    todo_chunk.clear();
    for (const gt::Chunk& c : m.chunks) {
      if (!ReflectBoxInRange(c.lo, c.hi)) continue;
      for (uint32_t k = 0; k < c.range_count; ++k) {
        todo.push_back(&m.ranges[c.first_range + k]);
        todo_base.push_back(c.base_vertex);
        todo_chunk.push_back(&c);
      }
    }
    order.resize(todo.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t c) { return todo[a]->material < todo[c]->material; });
    bound = -2;
    unsigned recorded = 0;
    for (uint32_t i : order) {
      const gt::Range& r = *todo[i];
      if (r.material != bound) {
        bound = r.material;
        bind_range(r, false);
      }
      ReflectReplay rr{};
      rr.blend = bd.blend;
      rr.depth = bd.depth;
      rr.srv[0] = bd.srv[0];
      rr.srv[1] = bd.srv[1];
      std::memcpy(rr.smp, bd.smp, sizeof(rr.smp));
      rr.vs = bd.vs;
      rr.ps = bd.ps;
      static const float kId[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
      std::memcpy(rr.world, kId, sizeof(rr.world));
      PSConstants pc = bd.pc;
      // No far-block switch in the cube (it works from the main camera's
      // screen position and focal length).
      for (float& f : pc.tfar) f = 0;
      std::memcpy(rr.pc, &pc, sizeof(PSConstants));
      rr.pc_size = sizeof(PSConstants);
      rr.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      rr.indexed = true;
      rr.nidx = r.index_count;
      rr.ioff = r.first_index * 2;
      rr.base_vertex = todo_base[i];
      const gt::Chunk& c = *todo_chunk[i];
      std::memcpy(rr.lo, c.lo, sizeof(rr.lo));
      std::memcpy(rr.hi, c.hi, sizeof(rr.hi));
      rr.bounded = true;
      rr.vb = terr_vb_;
      rr.ib = terr_ib_;
      ReflectRecord(rr);
      ++recorded;
    }
    if (st) st->reflect_records = recorded;
  }
  bd.pipe = false;  // our buffers are bound: the next draw binds the game's again
  {
    ID3D11ShaderResourceView* none = nullptr;
    b.ctx->PSSetShaderResources(6, 1, &none);
  }
  detail_fade_[0] = detail_fade_[1] = 0;
  for (float& f : terr_far_) f = 0;

  for (int i = 0; i < kRs; ++i) rs_[rs_keys[i]] = rs_save[i];
  tex_[0] = tex_save[0];
  tex_[1] = tex_save[1];
  std::memcpy(tss_, tss_save, sizeof(tss_save));
  if (cur_msaa_) MsaaMarkDrawn();
  if (has_depth) Note3DDraw();
  rt_->MarkGpuWritten();
  Counters().draws += draws;
  if (st) {
    st->chunks_drawn = drawn;
    st->tris_drawn = tris;
    st->draws = draws;
  }
  return true;
}

}  // namespace dd

