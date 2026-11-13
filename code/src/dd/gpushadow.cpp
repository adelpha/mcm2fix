// Rider shadows drawn by the graphics card; see gpushadow.h.
#include "gpushadow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "../config.h"
#include "../framerate.h"
#include "../patch.h"
#include "../smooth.h"
#include "backend.h"

namespace dd {
namespace {

template <typename T>
T Rd(uintptr_t a) {
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(v));
  return v;
}

// ---------------------------------------------------------------- the game

// ProjectedShadow (one per race, made when the track has a sun).
constexpr uintptr_t kPsVtable = 0x55C848;
constexpr uintptr_t kPsPreDrawSlot = kPsVtable + 0x30;  // 0x4df7f0: fit window, CPU raster, receivers
constexpr uintptr_t kPsDrawSlot = kPsVtable + 0x34;     // 0x4e0c00: upload the texture's dirty rectangle
constexpr uintptr_t kPsCasters = 0x2c, kPsNumCasters = 0x30;
constexpr uintptr_t kPsLightDir = 0x38;   // 3 floats, normalised (light mode 3)
constexpr uintptr_t kPsLightMode = 0x8c;  // 3 = directional (sun), 1 = point
constexpr uintptr_t kPsShade = 0x94;      // 16-bit shadow texel
constexpr uintptr_t kPsSrcBlend = 0x9c, kPsDstBlend = 0xa0, kPsFormat = 0xb0, kPsClear = 0xb4;
// Low byte: the terrain receiver (0x50f1af) draws with vertex alpha 255 - this
// (0x40 -> 0xbf), modulated with the texel's alpha.
constexpr uintptr_t kPsDiffuse = 0xac;
// Terrain receiver (vtable 0x55d658): draws the terrain under the window again with the texture.
constexpr uintptr_t kReceiverDrawSlot = 0x55D658 + 0x38;  // 0x50f910
// D3DIMSoultreeShadow (vtable 0x556624, rider self-shadow when the game's detail option asks for
// it): draw slot (0x4484c0; SimSmooth rebuilds it from the CPU raster there).
constexpr uintptr_t kSelfShadowDrawSlot = 0x556624 + 0x38;
// BikeRace: update (vtable slot 10) tells us which race is running; bike iterator.
constexpr uintptr_t kBikeRaceVtable = 0x555DDC;
constexpr uintptr_t kRaceUpdateSlot = kBikeRaceVtable + 4 * 10;
constexpr uintptr_t kRaceShadow = 0x5c;  // BikeRace -> its ProjectedShadow
constexpr uintptr_t kNextBike = 0x421420;
constexpr uintptr_t kBikeRider = 0x3bc, kBikeModel = 0x5c4, kModelObject = 0x1a0;
// D3DIMSoultreeObject (riders, bikes).
constexpr uintptr_t kObjVtable = 0x5564A4;
constexpr uintptr_t kObjDirty = 0x18c, kObjMatrix = 0xf8;
// Terrain patch draws and the calls to them (from the terrain node draw 0x481a40).
constexpr uintptr_t kPatchDraw = 0x481C00, kPatchDrawMT = 0x481F20;
constexpr uintptr_t kPatchCalls[] = {0x481B13, 0x481BAF};
constexpr uintptr_t kPatchMTCalls[] = {0x481AF9, 0x481BA8};

using SlotFn = int(__fastcall*)(void* self, void* edx);
using UpdateFn = void(__fastcall*)(uintptr_t self, void* edx, int arg);
using NextBikeFn = uintptr_t(__fastcall*)(uintptr_t self, void* edx, int* idx);
using GetMeshFn = void(__thiscall*)(void*, int, uintptr_t*, int*, uintptr_t*, int*, int, int);
using MeshCountFn = int(__thiscall*)(void*, int);
using MarkFn = void(__fastcall*)(void*);
using PatchFn = void(__fastcall*)(void* self, void* edx, int, int, int, int, int, int);
const GetMeshFn kGetMesh = reinterpret_cast<GetMeshFn>(0x444390);
const MeshCountFn kMeshCount = reinterpret_cast<MeshCountFn>(0x445FD0);
const MarkFn kMarkRebuild = reinterpret_cast<MarkFn>(0x502940);  // object and its children: rebuild next read

SlotFn g_prev_predraw = nullptr, g_prev_draw = nullptr, g_prev_recv_draw = nullptr, g_prev_self_draw = nullptr;
UpdateFn g_prev_update = nullptr;
uintptr_t g_race = 0;  // last BikeRace updated
int g_receiver = 0;    // inside a terrain patch draw

// ---------------------------------------------------------------- GPU side

constexpr int kGrid = 4;               // tiles per atlas row
constexpr int kTiles = kGrid * kGrid;  // most shadows at once
constexpr int kTilePx = 256;
constexpr int kAtlasPx = kGrid * kTilePx;
constexpr size_t kMaxVerts = 262144, kMaxIdx = 786432;
// How far along the sun's rays below the casters' top the ground is shaded
// (world units, metres): a rider jumping high still has a shadow, while a
// far hill behind the first ground the shadow falls on gets none.
constexpr float kReach = 60.0f;
// Soft shadows: the atlas has mipmaps down to kTopLod (a tile of 16 texels),
// and the receiver picks a level from the gap between the ground and the
// casters' lowest surface over it (depth below the top, up to kDepthRange).
constexpr int kTopLod = 4;
constexpr float kDepthRange = 32.0f;
// The gap is measured along the sun's rays, which are long when the sun is
// low (Baja: 20 degrees up, so a rider's head is ~12 units along the ray from
// its shadow). gpushadow-2 used 0.1 / 0.04: far too blurry.
constexpr float kBlurPerUnit = 0.015f;  // blur width per unit of gap, at ShadowSoftness=100
constexpr float kFadePerUnit = 0.008f;  // darkness / (1 + gap * this)
constexpr DXGI_FORMAT kAtlasFormat = DXGI_FORMAT_R16G16_FLOAT;

const char kAtlasShader[] = R"(
cbuffer P : register(b0) { float4 lrow[3]; float4 tile[16]; float4 depth; };
struct VIn { float3 pos : POSITION; uint id : TEXCOORD0; };
struct VOut { float4 pos : SV_Position; float h : TEXCOORD0; };
// The window always holds all of its casters (see GpuFrame), so nothing
// spills into a neighbouring tile. Depth: the surface furthest from the sun
// wins (GREATER), so h is the casters' underside over each texel.
VOut vs_main(VIn i) {
  int id = (int)i.id;
  float4 t = tile[id];
  float2 l = (float2(dot(i.pos, lrow[0].xyz), dot(i.pos, lrow[1].xyz)) - t.xy) * t.z;
  float h = max(dot(i.pos, lrow[2].xyz) - t.w, 0);
  float2 cell = float2((float)(id & 3), (float)(id >> 2));
  float2 uv = (cell + float2(0.5 + 0.5 * l.x, 0.5 - 0.5 * l.y)) * 0.25;
  VOut o;
  o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, saturate(h * depth.x), 1);
  o.h = h;
  return o;
}
// Coverage, and coverage times the depth below the top (resolve and mipmaps
// average both, so g / r is the mean depth of what covers a texel).
float4 ps_main(VOut i) : SV_Target { return float4(1, i.h, 0, 1); }
)";

struct AtlasConsts {
  float lrow[3][4];
  float tile[kTiles][4];
  float depth[4];  // x: 1 / kDepthRange
};
// Must match ShadowParams in the device shader (device.cpp).
struct RecvConsts {
  float lsm[16];
  float info[4];  // tiles, strength, atlas texel (uv), reach
  float soft[4];  // blur per unit gap (x 1/half), fade per unit gap, top level
  float tile[kTiles][4];
};
struct ShadowVertex {
  float pos[3];
  uint32_t tile;
};

struct Gpu {
  bool tried = false, ready = false;
  UINT samples = 1;
  ID3D11Texture2D* atlas = nullptr;  // single-sample, sampled by the terrain
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;   // the atlas itself, without MSAA
  ID3D11Texture2D* msaa = nullptr;         // drawn into, then resolved into the atlas
  ID3D11RenderTargetView* msaa_rtv = nullptr;
  ID3D11Texture2D* depth = nullptr;  // same samples as the target drawn into
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11DepthStencilState* ds = nullptr;  // GREATER: the casters' underside
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* layout = nullptr;
  ID3D11Buffer* cb = nullptr;       // AtlasConsts
  ID3D11Buffer* recv_cb = nullptr;  // RecvConsts
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT vb_bytes = 0, ib_bytes = 0;
  ID3D11SamplerState* sampler = nullptr;
  ID3D11RasterizerState* raster = nullptr;
} g_gpu;

bool MakeBuffer(UINT bytes, UINT bind, ID3D11Buffer** out) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = bytes;
  d.Usage = D3D11_USAGE_DYNAMIC;
  d.BindFlags = bind;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return SUCCEEDED(B().device->CreateBuffer(&d, nullptr, out));
}

bool EnsureGpu() {
  if (g_gpu.tried) return g_gpu.ready;
  g_gpu.tried = true;
  Backend& b = B();
  ID3DBlob* vs = Compile(kAtlasShader, "vs_main", "vs_4_0");
  ID3DBlob* ps = Compile(kAtlasShader, "ps_main", "ps_4_0");
  if (!vs || !ps) {
    SafeRelease(vs);
    SafeRelease(ps);
    Log("rider shadows: shadow atlas shaders did not compile; the game's own shadow stays");
    return false;
  }
  b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_gpu.vs);
  b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_gpu.ps);
  const D3D11_INPUT_ELEMENT_DESC el[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  b.device->CreateInputLayout(el, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &g_gpu.layout);
  vs->Release();
  ps->Release();

  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = kAtlasPx;
  td.MipLevels = kTopLod + 1;
  td.ArraySize = 1;
  td.Format = kAtlasFormat;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  HRESULT hr = b.device->CreateTexture2D(&td, nullptr, &g_gpu.atlas);
  if (g_gpu.atlas) {
    b.device->CreateShaderResourceView(g_gpu.atlas, nullptr, &g_gpu.srv);
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = kAtlasFormat;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rv.Texture2D.MipSlice = 0;
    b.device->CreateRenderTargetView(g_gpu.atlas, &rv, &g_gpu.rtv);
  } else {
    Log("rider shadows: atlas texture (%dx%d, %d levels, R16G16_FLOAT) not created: %08lx", kAtlasPx, kAtlasPx,
        kTopLod + 1, static_cast<unsigned long>(hr));
  }
  UINT q = 0;
  if (SUCCEEDED(b.device->CheckMultisampleQualityLevels(kAtlasFormat, 4, &q)) && q > 0) {
    D3D11_TEXTURE2D_DESC md = td;
    md.MipLevels = 1;
    md.MiscFlags = 0;
    md.SampleDesc.Count = 4;
    md.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(b.device->CreateTexture2D(&md, nullptr, &g_gpu.msaa)) &&
        SUCCEEDED(b.device->CreateRenderTargetView(g_gpu.msaa, nullptr, &g_gpu.msaa_rtv))) {
      g_gpu.samples = 4;
    } else {
      SafeRelease(g_gpu.msaa);
    }
  } else {
    Log("rider shadows: no 4x MSAA for the atlas format; edges drawn without it");
  }
  {
    D3D11_TEXTURE2D_DESC dd = td;
    dd.MipLevels = 1;
    dd.MiscFlags = 0;
    dd.Format = DXGI_FORMAT_D16_UNORM;
    dd.SampleDesc.Count = g_gpu.samples;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(b.device->CreateTexture2D(&dd, nullptr, &g_gpu.depth)))
      b.device->CreateDepthStencilView(g_gpu.depth, nullptr, &g_gpu.dsv);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    b.device->CreateDepthStencilState(&dsd, &g_gpu.ds);
    if (!g_gpu.dsv || !g_gpu.ds) Log("rider shadows: atlas depth buffer not created");
  }
  D3D11_BUFFER_DESC cd{};
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cd.ByteWidth = sizeof(AtlasConsts);
  b.device->CreateBuffer(&cd, nullptr, &g_gpu.cb);
  cd.ByteWidth = sizeof(RecvConsts);
  b.device->CreateBuffer(&cd, nullptr, &g_gpu.recv_cb);
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  b.device->CreateSamplerState(&sd, &g_gpu.sampler);
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = FALSE;
  rd.MultisampleEnable = TRUE;
  b.device->CreateRasterizerState(&rd, &g_gpu.raster);
  g_gpu.ready = g_gpu.vs && g_gpu.ps && g_gpu.layout && g_gpu.srv && g_gpu.rtv && g_gpu.cb && g_gpu.recv_cb &&
                g_gpu.sampler && g_gpu.raster && g_gpu.dsv && g_gpu.ds;
  Log("rider shadows: GPU shadow atlas %s (%dx%d R16G16_FLOAT, %d levels, %d tiles of %d px, %u samples, "
      "depth buffer %s)",
      g_gpu.ready ? "ready" : "FAILED, the game's own shadow stays", kAtlasPx, kAtlasPx, kTopLod + 1, kTiles, kTilePx,
      g_gpu.samples, g_gpu.dsv ? "yes" : "no");
  return g_gpu.ready;
}

// ---------------------------------------------------------------- per frame

struct Group {
  uintptr_t key = 0;     // the bike (or the first caster)
  uintptr_t casters[2] = {};
  int n = 0;
  float dist2 = 0;       // from the rider the camera follows
  bool followed = false;
};

struct Window {
  float half = 0;  // half the tile's side, world units
  unsigned seen = 0;
};
std::unordered_map<uintptr_t, Window> g_windows;

struct Frame {
  bool active = false;       // the atlas holds this frame's shadows
  RecvConsts recv{};
  bool recv_dirty = true;
  unsigned bound_epoch = ~0u;
  float light[16] = {};      // world -> light space (row vectors)
  float strength = 0.5f;
} g_f;

std::vector<ShadowVertex> g_verts;
std::vector<uint32_t> g_idx;
std::vector<Group> g_groups;
unsigned g_frame_no = 0;
uintptr_t g_strength_ps = 0;  // ProjectedShadow the strength was worked out for
uintptr_t g_logged_race = 0;
unsigned g_race_frames = 0;   // GPU frames since this race's first

// ShadowLog counters (5 s windows).
struct ShadowStats {
  unsigned frames = 0, gpu = 0, game = 0;
  unsigned why_off = 0, why_d3d9 = 0, why_notready = 0, why_light = 0, why_gpu = 0;
  unsigned groups = 0, groups_max = 0, tiles = 0, verts = 0, tris = 0, no_race = 0, race_mismatch = 0;
  unsigned bad_vtable = 0, empty_groups = 0, dropped = 0, too_many_verts = 0, bad_index = 0;
  double collect_ms = 0, render_ms = 0;
  unsigned recv_shadowed = 0, recv_multiply = 0, recv_inactive = 0, recv_patches = 0;
  unsigned game_draw_skipped = 0, recv_draw_skipped = 0, self_draw_skipped = 0;
  float half_min = 1e9f, half_max = 0;
  double last_log = 0;
} g_st;
uintptr_t g_bad_vtable_seen = 0;

double NowMs() {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool IsObject(uintptr_t o) {
  if (!o) return false;
  const uintptr_t vt = Rd<uintptr_t>(o);
  if (vt == kObjVtable) return true;
  ++g_st.bad_vtable;
  if (g_bad_vtable_seen != vt) {
    g_bad_vtable_seen = vt;
    Log("rider shadows: caster %08x has vtable %08x, not a model (%08x); skipped", static_cast<unsigned>(o),
        static_cast<unsigned>(vt), static_cast<unsigned>(kObjVtable));
  }
  return false;
}

// How dark the game's shadow is (0 = none, 1 = black), from its texture
// settings (0x4df4d0): with a multiplying blend (source DESTCOLOR) the texel
// is a grey level, otherwise black with an alpha.
float GameStrength(uintptr_t ps, char* why, size_t why_size) {
  const unsigned shade = Rd<uint16_t>(ps + kPsShade);
  const int src = Rd<int>(ps + kPsSrcBlend), dst = Rd<int>(ps + kPsDstBlend), fmt = Rd<int>(ps + kPsFormat);
  const unsigned clear = Rd<uint16_t>(ps + kPsClear);
  float s;
  if (fmt == 0x115c && src == 9) {
    s = 1.0f - static_cast<float>(shade & 0xf) / 15.0f;
  } else if (fmt == 0x115c) {
    s = static_cast<float>((shade >> 12) & 0xf) / 15.0f;
  } else {
    s = 0.5f;
  }
  // The receiver's vertex alpha scales an alpha-blended shadow (format 0x613: 0x7f).
  const unsigned diffuse = Rd<uint32_t>(ps + kPsDiffuse);
  float va = 1.0f;
  if (src != 9) va = fmt == 0x613 ? 127.0f / 255.0f : static_cast<float>(255 - (diffuse & 0xff)) / 255.0f;
  const float texel = s;
  s *= va;
  std::snprintf(why, why_size, "shade %04x clear %04x format %x blend %d/%d -> texel %.2f x vertex alpha %.2f = %.2f",
                shade, clear, fmt, src, dst, texel, va, s);
  return std::min(0.85f, std::max(0.15f, s));
}

void Normalize(float v[3]) {
  const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (n > 1e-6f) v[0] /= n, v[1] /= n, v[2] /= n;
}

// Light space: x, y across the sun's view, z along its rays (larger = further
// from the sun). Row-vector matrix: [x y z 1] * M.
bool LightBasis(uintptr_t ps, float m[16], float dir_out[3], bool* flipped) {
  float d[3] = {Rd<float>(ps + kPsLightDir), Rd<float>(ps + kPsLightDir + 4), Rd<float>(ps + kPsLightDir + 8)};
  if (!(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-6f)) return false;
  Normalize(d);
  *flipped = d[1] > 0;  // y is up: the rays go down
  if (*flipped) d[0] = -d[0], d[1] = -d[1], d[2] = -d[2];
  const float a[3] = {std::fabs(d[0]) < 0.9f ? 1.0f : 0.0f, 0.0f, std::fabs(d[0]) < 0.9f ? 0.0f : 1.0f};
  float u[3] = {a[1] * d[2] - a[2] * d[1], a[2] * d[0] - a[0] * d[2], a[0] * d[1] - a[1] * d[0]};
  Normalize(u);
  float v[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
  std::memset(m, 0, 16 * sizeof(float));
  for (int r = 0; r < 3; ++r) {
    m[r * 4 + 0] = u[r];
    m[r * 4 + 1] = v[r];
    m[r * 4 + 2] = d[r];
  }
  m[15] = 1;
  std::memcpy(dir_out, d, sizeof(d));
  return true;
}

// The bikes to shadow: every bike in the race (RiderShadows=2), or the casters
// the game itself picked (the rider the camera follows).
void CollectGroups(uintptr_t ps, bool all) {
  g_groups.clear();
  const int nc = Rd<int>(ps + kPsNumCasters);
  const uintptr_t clist = Rd<uintptr_t>(ps + kPsCasters);
  const uintptr_t followed = (nc > 0 && clist) ? Rd<uintptr_t>(clist) : 0;
  float fpos[3] = {};
  if (followed && IsObject(followed))
    for (int k = 0; k < 3; ++k) fpos[k] = Rd<float>(followed + kObjMatrix + 0x30 + 4 * k);
  bool from_race = false;
  if (all) {
    {
      from_race = true;
      const auto next = reinterpret_cast<NextBikeFn>(kNextBike);
      int idx = 0;
      for (int guard = 0; guard < 64; ++guard) {
        const uintptr_t bike = next(g_race, nullptr, &idx);
        if (!bike) break;
        Group g;
        g.key = bike;
        const uintptr_t rider = Rd<uintptr_t>(bike + kBikeRider);
        const uintptr_t model = Rd<uintptr_t>(bike + kBikeModel);
        const uintptr_t body = model ? Rd<uintptr_t>(model + kModelObject) : 0;
        if (IsObject(rider)) g.casters[g.n++] = rider;
        if (IsObject(body)) g.casters[g.n++] = body;
        if (!g.n) continue;
        g.followed = rider == followed || body == followed;
        g_groups.push_back(g);
        if (idx == 0) break;  // the iterator wrapped
      }
      // None listed (the podium cutscene): the game's own pick.
      from_race = !g_groups.empty();
    }
  }
  if (!from_race && clist && nc > 0 && nc <= 8) {
    Group g;
    g.key = followed;
    g.followed = true;
    for (int i = 0; i < nc && g.n < 2; ++i) {
      const uintptr_t c = Rd<uintptr_t>(clist + 4 * i);
      if (IsObject(c)) g.casters[g.n++] = c;
    }
    if (g.n) g_groups.push_back(g);
  }
  for (Group& g : g_groups) {
    const uintptr_t c = g.casters[0];
    float d2 = 0;
    for (int k = 0; k < 3; ++k) {
      const float d = Rd<float>(c + kObjMatrix + 0x30 + 4 * k) - fpos[k];
      d2 += d * d;
    }
    g.dist2 = g.followed ? -1.0f : d2;
  }
  std::sort(g_groups.begin(), g_groups.end(), [](const Group& a, const Group& b) { return a.dist2 < b.dist2; });
  if (g_groups.size() > static_cast<size_t>(kTiles)) {
    g_st.dropped += static_cast<unsigned>(g_groups.size()) - kTiles;
    g_groups.resize(kTiles);
  }
}


// One group's meshes into g_verts/g_idx as tile `tile`; returns false if empty.
// Bounds in light space go to lo/hi (x, y, z).
bool AddGroup(const Group& g, uint32_t tile, float lo[3], float hi[3]) {
  const bool blended = SmoothPartsActive();
  const size_t v0 = g_verts.size();
  for (int k = 0; k < 3; ++k) lo[k] = 1e30f, hi[k] = -1e30f;
  for (int ci = 0; ci < g.n; ++ci) {
    void* c = reinterpret_cast<void*>(g.casters[ci]);
    // While drawing between steps, have the vertices rebuilt from the
    // blended parts (smooth_parts.cpp), as the drawn rider is.
    if (blended) *reinterpret_cast<int*>(g.casters[ci] + kObjDirty) = 1;
    const int meshes = kMeshCount(c, -1);
    if (meshes <= 0 || meshes > 256) continue;
    for (int j = 0; j < meshes; ++j) {
      uintptr_t verts = 0, idx = 0;
      int count = 0, nidx = 0;
      kGetMesh(c, j, &verts, &count, &idx, &nidx, 0, -1);
      if (!verts || !idx || count <= 0 || count > 65536 || nidx <= 0) continue;
      if (g_verts.size() + count > kMaxVerts || g_idx.size() + nidx > kMaxIdx) {
        ++g_st.too_many_verts;
        break;
      }
      const uint32_t base = static_cast<uint32_t>(g_verts.size());
      for (int i = 0; i < count; ++i) {
        const float* p = reinterpret_cast<const float*>(verts + static_cast<uintptr_t>(i) * 32);
        ShadowVertex sv{{p[0], p[1], p[2]}, tile};
        g_verts.push_back(sv);
        for (int k = 0; k < 3; ++k) {
          const float* m = g_f.light;
          const float l = p[0] * m[k] + p[1] * m[4 + k] + p[2] * m[8 + k];
          lo[k] = std::min(lo[k], l);
          hi[k] = std::max(hi[k], l);
        }
      }
      const auto* ix = reinterpret_cast<const uint16_t*>(idx);
      const int tris = nidx / 3;
      for (int t = 0; t < tris; ++t) {
        const uint16_t a = ix[t * 3], b = ix[t * 3 + 1], cc = ix[t * 3 + 2];
        if (a >= count || b >= count || cc >= count) {
          ++g_st.bad_index;
          continue;
        }
        g_idx.push_back(base + a);
        g_idx.push_back(base + b);
        g_idx.push_back(base + cc);
      }
    }
    // As the game does after reading a caster: the next reader (the draw,
    // or the simulation) rebuilds the vertices from the step.
    kMarkRebuild(c);
  }
  return g_verts.size() > v0 && hi[0] > lo[0];
}

void RenderAtlas(int tiles, const AtlasConsts& ac) {
  Backend& b = B();
  ID3D11DeviceContext* ctx = b.ctx;
  const UINT vbytes = static_cast<UINT>(g_verts.size() * sizeof(ShadowVertex));
  const UINT ibytes = static_cast<UINT>(g_idx.size() * sizeof(uint32_t));
  if (vbytes > g_gpu.vb_bytes) {
    SafeRelease(g_gpu.vb);
    g_gpu.vb_bytes = std::max<UINT>(vbytes * 3 / 2, 64 * 1024);
    if (!MakeBuffer(g_gpu.vb_bytes, D3D11_BIND_VERTEX_BUFFER, &g_gpu.vb)) g_gpu.vb_bytes = 0;
  }
  if (ibytes > g_gpu.ib_bytes) {
    SafeRelease(g_gpu.ib);
    g_gpu.ib_bytes = std::max<UINT>(ibytes * 3 / 2, 64 * 1024);
    if (!MakeBuffer(g_gpu.ib_bytes, D3D11_BIND_INDEX_BUFFER, &g_gpu.ib)) g_gpu.ib_bytes = 0;
  }
  ID3D11RenderTargetView* rtv = g_gpu.msaa_rtv ? g_gpu.msaa_rtv : g_gpu.rtv;
  ID3D11ShaderResourceView* none = nullptr;
  ctx->PSSetShaderResources(3, 1, &none);  // the atlas is about to be written
  const float zero[4] = {0, 0, 0, 0};
  ctx->OMSetRenderTargets(1, &rtv, g_gpu.dsv);
  ctx->ClearRenderTargetView(rtv, zero);
  ctx->ClearDepthStencilView(g_gpu.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
  if (tiles > 0 && !g_idx.empty() && g_gpu.vb && g_gpu.ib) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_gpu.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      std::memcpy(m.pData, g_verts.data(), vbytes);
      ctx->Unmap(g_gpu.vb, 0);
    }
    if (SUCCEEDED(ctx->Map(g_gpu.ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
      std::memcpy(m.pData, g_idx.data(), ibytes);
      ctx->Unmap(g_gpu.ib, 0);
    }
    ctx->UpdateSubresource(g_gpu.cb, 0, nullptr, &ac, 0, 0);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(kAtlasPx), static_cast<float>(kAtlasPx), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_gpu.raster);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(g_gpu.ds, 0);
    ctx->IASetInputLayout(g_gpu.layout);
    const UINT stride = sizeof(ShadowVertex), off = 0;
    ctx->IASetVertexBuffers(0, 1, &g_gpu.vb, &stride, &off);
    ctx->IASetIndexBuffer(g_gpu.ib, DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_gpu.vs, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_gpu.cb);
    ctx->PSSetShader(g_gpu.ps, nullptr, 0);
    ctx->DrawIndexed(static_cast<UINT>(g_idx.size()), 0, 0);
  }
  ID3D11RenderTargetView* nort = nullptr;
  ctx->OMSetRenderTargets(1, &nort, nullptr);
  if (g_gpu.msaa_rtv) ctx->ResolveSubresource(g_gpu.atlas, 0, g_gpu.msaa, 0, kAtlasFormat);
  ctx->GenerateMips(g_gpu.srv);
  // The device binds everything again on its next draw (and so do we, below).
  ContextTouched();
}

// The game's shadow pre-draw, done on the GPU. False: let the game do it.
bool GpuFrame(uintptr_t ps) {
  const Config& c = GetConfig();
  if (c.rider_shadows <= 0) { ++g_st.why_off; return false; }
  if (IsD3D9()) { ++g_st.why_d3d9; return false; }
  if (!Ready()) { ++g_st.why_notready; return false; }
  if (Rd<int>(ps + kPsLightMode) != 3) {
    ++g_st.why_light;
    LogOnce("rider shadows: the track's shadow light is not a sun (mode %d); the game's own shadow is drawn",
            Rd<int>(ps + kPsLightMode));
    return false;
  }
  // Only the race's own shadow (the garage has one too, for its bike).
  if (!g_race || Rd<uintptr_t>(g_race) != kBikeRaceVtable) { ++g_st.no_race; return false; }
  if (Rd<uintptr_t>(g_race + kRaceShadow) != ps) { ++g_st.race_mismatch; return false; }
  DD_LOCK;
  if (!EnsureGpu()) { ++g_st.why_gpu; return false; }
  const double t0 = NowMs();
  float dir[3];
  bool flipped = false;
  if (!LightBasis(ps, g_f.light, dir, &flipped)) {
    ++g_st.why_light;
    LogOnce("rider shadows: the shadow light has no direction; the game's own shadow is drawn");
    return false;
  }
  if (g_strength_ps != ps) {
    g_strength_ps = ps;
    char why[160];
    g_f.strength = GameStrength(ps, why, sizeof(why));
    if (c.shadow_strength > 0) g_f.strength = std::min(100, c.shadow_strength) / 100.0f;
    Log("rider shadows: race shadow %08x: sun direction (%.3f %.3f %.3f)%s; game's darkness: %s; using %.2f%s; "
        "softness %d%% (blur %.3f and fade %.3f per unit of height, up to level %d)",
        static_cast<unsigned>(ps), Rd<float>(ps + kPsLightDir), Rd<float>(ps + kPsLightDir + 4),
        Rd<float>(ps + kPsLightDir + 8), flipped ? " (pointed up, turned round)" : "", why, g_f.strength,
        c.shadow_strength > 0 ? " (ShadowStrength)" : "", c.shadow_softness, kBlurPerUnit * c.shadow_softness / 100.0f,
        kFadePerUnit * c.shadow_softness / 100.0f, kTopLod);
  }
  CollectGroups(ps, c.rider_shadows >= 2);
  g_verts.clear();
  g_idx.clear();
  AtlasConsts ac{};
  RecvConsts& rc = g_f.recv;
  for (int k = 0; k < 3; ++k)
    for (int r = 0; r < 3; ++r) ac.lrow[k][r] = g_f.light[r * 4 + k];
  ac.depth[0] = 1.0f / kDepthRange;
  int tiles = 0;
  ++g_frame_no;
  for (const Group& g : g_groups) {
    float lo[3], hi[3];
    if (!AddGroup(g, static_cast<uint32_t>(tiles), lo, hi)) {
      ++g_st.empty_groups;
      continue;
    }
    // A square window around the casters. It only grows at once; it shrinks
    // when the casters fit in well under it, so it seldom changes size
    // (each change moves the shadow's texels).
    const float need = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f * 1.08f + 0.05f;
    Window& w = g_windows[g.key];
    if (w.half < need || need < w.half * 0.6f) w.half = need * 1.15f;
    w.seen = g_frame_no;
    const float texel = 2.0f * w.half / kTilePx;
    // Centre on a whole texel, so still parts of the shadow stay still.
    const float cx = std::floor((lo[0] + hi[0]) * 0.5f / texel + 0.5f) * texel;
    const float cy = std::floor((lo[1] + hi[1]) * 0.5f / texel + 0.5f) * texel;
    float* t = ac.tile[tiles];
    t[0] = cx;
    t[1] = cy;
    t[2] = 1.0f / w.half;
    t[3] = lo[2];  // the casters' top: ground nearer the sun than this is not shaded
    std::memcpy(rc.tile[tiles], t, sizeof(float) * 4);
    g_st.half_min = std::min(g_st.half_min, w.half);
    g_st.half_max = std::max(g_st.half_max, w.half);
    if (c.shadow_log >= 2 && g_race_frames >= 600 && g_race_frames < 690) {
      Log("shadow trace: frame %u tile %d bike %08x%s casters %d: centre (%.3f %.3f) half %.3f top %.3f bounds x %.3f..%.3f "
          "y %.3f..%.3f z %.3f..%.3f, %u verts so far",
          g_race_frames, tiles, static_cast<unsigned>(g.key), g.followed ? " (camera)" : "", g.n, cx, cy, w.half,
          lo[2], lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], static_cast<unsigned>(g_verts.size()));
    }
    ++tiles;
  }
  const double t1 = NowMs();
  RenderAtlas(tiles, ac);
  const double t2 = NowMs();
  rc.info[0] = static_cast<float>(tiles);
  rc.info[1] = g_f.strength;
  rc.info[2] = 1.0f / kAtlasPx;
  rc.info[3] = kReach;
  const float soft = c.shadow_softness / 100.0f;
  rc.soft[0] = kBlurPerUnit * soft * kTilePx * 0.5f;  // blur / level-0 texel = gap * this * (1 / half)
  rc.soft[1] = kFadePerUnit * soft;
  rc.soft[2] = static_cast<float>(kTopLod);
  rc.soft[3] = 0;
  g_f.recv_dirty = true;
  g_f.bound_epoch = ~0u;
  g_f.active = true;

  ++g_st.gpu;
  ++g_race_frames;
  g_st.groups += static_cast<unsigned>(g_groups.size());
  g_st.groups_max = std::max(g_st.groups_max, static_cast<unsigned>(g_groups.size()));
  g_st.tiles += tiles;
  g_st.verts += static_cast<unsigned>(g_verts.size());
  g_st.tris += static_cast<unsigned>(g_idx.size() / 3);
  g_st.collect_ms += t1 - t0;
  g_st.render_ms += t2 - t1;
  if (g_logged_race != ps) {
    g_logged_race = ps;
    g_race_frames = 0;
    Log("rider shadows: drawing on the GPU: %d shadows (%s), %u vertices, %u triangles; race %08x%s; light basis "
        "u (%.3f %.3f %.3f) v (%.3f %.3f %.3f) d (%.3f %.3f %.3f); casters picked by the game: %d",
        tiles, c.rider_shadows >= 2 ? "all riders" : "the rider the camera follows",
        static_cast<unsigned>(g_verts.size()), static_cast<unsigned>(g_idx.size() / 3),
        static_cast<unsigned>(g_race), "",
        g_f.light[0], g_f.light[4], g_f.light[8], g_f.light[1], g_f.light[5], g_f.light[9], dir[0], dir[1], dir[2],
        Rd<int>(ps + kPsNumCasters));
    const int nr = Rd<int>(ps + 0x128);
    const uintptr_t rl = Rd<uintptr_t>(ps + 0x12c);
    for (int i = 0; i < nr && i < 16 && rl; ++i) {
      const uintptr_t r = Rd<uintptr_t>(rl + 4 * i);
      Log("rider shadows:   game shadow receiver %d: %08x vtable %08x (%s), now not drawn", i, static_cast<unsigned>(r),
          static_cast<unsigned>(r ? Rd<uintptr_t>(r) : 0),
          r && Rd<uintptr_t>(r) == 0x55D658 ? "terrain" : (r && Rd<uintptr_t>(r) == 0x556624 ? "rider self-shadow" : "?"));
    }
    for (size_t i = 0; i < g_groups.size() && i < static_cast<size_t>(kTiles); ++i) {
      const Group& g = g_groups[i];
      Log("rider shadows:   bike %08x%s: casters %08x %08x, %.1f units from the camera's rider", static_cast<unsigned>(g.key),
          g.followed ? " (camera)" : "", static_cast<unsigned>(g.casters[0]), static_cast<unsigned>(g.casters[1]),
          g.dist2 > 0 ? std::sqrt(g.dist2) : 0.0f);
    }
  }
  return true;
}

// ---------------------------------------------------------------- hooks

int __fastcall PreDrawHook(void* self, void* edx) {
  if (GpuFrame(reinterpret_cast<uintptr_t>(self))) return 1;
  ++g_st.game;
  return g_prev_predraw(self, edx);
}

int __fastcall DrawHook(void* self, void* edx) {
  if (g_f.active) {
    ++g_st.game_draw_skipped;
    return 1;
  }
  return g_prev_draw(self, edx);
}

int __fastcall ReceiverDrawHook(void* self, void* edx) {
  if (g_f.active) {
    ++g_st.recv_draw_skipped;
    return 1;
  }
  return g_prev_recv_draw(self, edx);
}

int __fastcall SelfShadowDrawHook(void* self, void* edx) {
  if (g_f.active) {
    ++g_st.self_draw_skipped;
    return 1;
  }
  return g_prev_self_draw(self, edx);
}

// The race's sun, noted at each race update (ShadowSunDirection).
struct SunNote {
  bool ok = false;
  float dir[3] = {};
} g_sun;

void NoteSun(uintptr_t race) {
  const uintptr_t ps = Rd<uintptr_t>(race + kRaceShadow);
  float d[3] = {};
  if (ps && Rd<uintptr_t>(ps) == kPsVtable && Rd<int>(ps + kPsLightMode) == 3)
    for (int k = 0; k < 3; ++k) d[k] = Rd<float>(ps + kPsLightDir + 4 * k);
  g_sun.ok = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-6f;  // no sun on this track: none
  if (!g_sun.ok) return;
  Normalize(d);
  if (d[1] > 0) d[0] = -d[0], d[1] = -d[1], d[2] = -d[2];  // the rays go down (y is up)
  for (int k = 0; k < 3; ++k) g_sun.dir[k] = -d[k];
  g_sun.ok = true;
}

void __fastcall RaceUpdateHook(uintptr_t self, void* edx, int arg) {
  g_race = self;
  NoteSun(self);
  g_prev_update(self, edx, arg);
}

void __fastcall PatchHook(void* self, void* edx, int a, int b, int c, int d, int e, int f) {
  ++g_receiver;
  ++g_st.recv_patches;
  reinterpret_cast<PatchFn>(kPatchDraw)(self, edx, a, b, c, d, e, f);
  --g_receiver;
}

void __fastcall PatchMTHook(void* self, void* edx, int a, int b, int c, int d, int e, int f) {
  ++g_receiver;
  ++g_st.recv_patches;
  reinterpret_cast<PatchFn>(kPatchDrawMT)(self, edx, a, b, c, d, e, f);
  --g_receiver;
}

bool ChainSlot(uintptr_t slot, void* fn, SlotFn* prev, const char* what) {
  const uintptr_t old = Rd<uintptr_t>(slot);
  if (old < 0x401000 || old > 0x7fffffff) {
    Log("rider shadows: %s slot %08x holds %08x; not hooked", what, static_cast<unsigned>(slot),
        static_cast<unsigned>(old));
    return false;
  }
  *prev = reinterpret_cast<SlotFn>(old);
  patch::Write(slot, &fn, sizeof(fn));
  return true;
}

bool PatchCallSite(uintptr_t site, uintptr_t original, const void* hook) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
  if (p[0] != 0xE8 || site + 5 + Rd<int32_t>(site + 1) != original) {
    Log("rider shadows: call at %08x is not to %08x, not hooked", static_cast<unsigned>(site),
        static_cast<unsigned>(original));
    return false;
  }
  patch::Call(site, hook, 5);
  return true;
}

}  // namespace

void InstallGpuShadows() {
  const Config& c = GetConfig();
  // The race update hook only notes which race runs; reflections use it for
  // the sun's direction (ShadowSunDirection) whatever RiderShadows says.
  SlotFn prev_update = nullptr;
  const bool race = ChainSlot(kRaceUpdateSlot, reinterpret_cast<void*>(&RaceUpdateHook), &prev_update, "race update");
  g_prev_update = reinterpret_cast<UpdateFn>(reinterpret_cast<void*>(prev_update));
  if (c.rider_shadows <= 0) {
    Log("rider shadows: RiderShadows=0, the game's own shadow; race update %s", race ? "hooked" : "NOT hooked");
    return;
  }
  bool ok = ChainSlot(kPsPreDrawSlot, reinterpret_cast<void*>(&PreDrawHook), &g_prev_predraw, "shadow pre-draw");
  ok = ok && ChainSlot(kPsDrawSlot, reinterpret_cast<void*>(&DrawHook), &g_prev_draw, "shadow draw");
  ok = ok && ChainSlot(kReceiverDrawSlot, reinterpret_cast<void*>(&ReceiverDrawHook), &g_prev_recv_draw,
                       "terrain receiver draw");
  ok = ok && ChainSlot(kSelfShadowDrawSlot, reinterpret_cast<void*>(&SelfShadowDrawHook), &g_prev_self_draw,
                       "rider self-shadow draw");
  int patches = 0;
  for (uintptr_t s : kPatchCalls) patches += PatchCallSite(s, kPatchDraw, reinterpret_cast<const void*>(&PatchHook));
  for (uintptr_t s : kPatchMTCalls)
    patches += PatchCallSite(s, kPatchDrawMT, reinterpret_cast<const void*>(&PatchMTHook));
  Log("rider shadows: RiderShadows=%d: %s; %d of 4 terrain patch calls hooked; race update %s (Direct3D 11 only; "
      "Direct3D 9 keeps the game's shadow)",
      c.rider_shadows, ok ? (c.rider_shadows >= 2 ? "every rider's shadow drawn on the GPU" : "the camera rider's shadow drawn on the GPU")
                          : "hooks MISSING, the game's own shadow",
      patches, race ? "hooked" : "NOT hooked (only the camera rider)");
}

bool ShadowReceiverNow() { return g_receiver > 0; }

bool ShadowSunDirection(float to_sun[3]) {
  // Noted at the race's update (the race's memory may be gone by now, so it
  // is not read here); kept while paused and after the race.
  if (!g_sun.ok) return false;
  std::memcpy(to_sun, g_sun.dir, sizeof(g_sun.dir));
  return true;
}

bool ShadowBindReceiver(const float* world, bool blend, DWORD src, DWORD dst) {
  if (!g_f.active || !g_gpu.ready) {
    ++g_st.recv_inactive;
    return false;
  }
  // A layer that multiplies or adds onto the ground (detail texture pass)
  // already gets the shadow from the ground under it.
  if (blend && (src == 9 /*DESTCOLOR*/ || src == 1 /*ZERO*/ || dst == 3 /*SRCCOLOR*/ || dst == 2 /*ONE*/)) {
    ++g_st.recv_multiply;
    return false;
  }
  RecvConsts& rc = g_f.recv;
  float lsm[16];
  for (int r = 0; r < 4; ++r)
    for (int col = 0; col < 4; ++col) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += world[r * 4 + k] * g_f.light[k * 4 + col];
      lsm[r * 4 + col] = s;
    }
  if (std::memcmp(lsm, rc.lsm, sizeof(lsm)) != 0) {
    std::memcpy(rc.lsm, lsm, sizeof(lsm));
    g_f.recv_dirty = true;
  }
  ID3D11DeviceContext* ctx = B().ctx;
  if (g_f.recv_dirty) {
    ctx->UpdateSubresource(g_gpu.recv_cb, 0, nullptr, &rc, 0, 0);
    g_f.recv_dirty = false;
  }
  if (g_f.bound_epoch != ContextEpoch()) {
    ctx->VSSetConstantBuffers(2, 1, &g_gpu.recv_cb);
    ctx->PSSetConstantBuffers(2, 1, &g_gpu.recv_cb);
    ctx->PSSetShaderResources(3, 1, &g_gpu.srv);
    ctx->PSSetSamplers(3, 1, &g_gpu.sampler);
    g_f.bound_epoch = ContextEpoch();
  }
  ++g_st.recv_shadowed;
  return true;
}

void ShadowFrameEnd() {
  g_f.active = false;
  ++g_st.frames;
  // Forget windows of bikes not seen for a while.
  if ((g_st.frames & 1023) == 0) {
    for (auto it = g_windows.begin(); it != g_windows.end();)
      it = g_frame_no - it->second.seen > 600 ? g_windows.erase(it) : std::next(it);
  }
  const Config& c = GetConfig();
  if (!c.shadow_log) return;
  const double now = NowMs();
  if (g_st.last_log == 0) g_st.last_log = now;
  if (now - g_st.last_log < 5000) return;
  if (g_st.gpu || g_st.game) {
    const double n = g_st.gpu ? g_st.gpu : 1;
    Log("rider shadows: %u frames: %u drawn on the GPU, %u by the game (off %u, Direct3D 9 %u, device not ready %u, "
        "no sun %u, GPU setup failed %u); per GPU frame: %.1f bikes (most %u), %.1f shadows, %.0f vertices, %.0f "
        "triangles, collect %.3f ms, atlas %.3f ms; window half size %.2f..%.2f",
        g_st.frames, g_st.gpu, g_st.game, g_st.why_off, g_st.why_d3d9, g_st.why_notready, g_st.why_light,
        g_st.why_gpu, g_st.groups / n, g_st.groups_max, g_st.tiles / n, g_st.verts / n, g_st.tris / n,
        g_st.collect_ms / n, g_st.render_ms / n, g_st.half_min < 1e8f ? g_st.half_min : 0.0f, g_st.half_max);
    Log("rider shadows:   terrain: %.1f patch draws/frame, %.1f draws shadowed, %.1f left alone as multiplying "
        "layers, %u outside GPU frames; skipped game draws: texture %u, receiver %u, self-shadow %u; race not found "
        "%u, race/shadow mismatch %u, non-model casters %u, empty bikes %u, over %d bikes %u, vertex cap %u, bad "
        "indices %u",
        g_st.recv_patches / n, g_st.recv_shadowed / n, g_st.recv_multiply / n, g_st.recv_inactive,
        g_st.game_draw_skipped, g_st.recv_draw_skipped, g_st.self_draw_skipped, g_st.no_race, g_st.race_mismatch,
        g_st.bad_vtable, g_st.empty_groups, kTiles, g_st.dropped, g_st.too_many_verts, g_st.bad_index);
  }
  const double last = now;
  g_st = ShadowStats{};
  g_st.last_log = last;
}

}  // namespace dd
