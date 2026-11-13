// Reflections on bikes and helmets (ini Reflections; Direct3D 11 only).
//
// The game lights bikes and riders itself and sends them pre-lit (FVF 0x1e2:
// position, colour, specular, one texture), so there are no normals to use.
// We work them out from the mesh: each draw's triangles, with vertices at the
// same place welded so the shading is smooth across texture seams.
//
// Which surfaces reflect:
//  - bikes: the bike's own texture (one 256x256 picture per bike, e.g.
//    hondacr250_08), by name pattern (ReflectBikeTextures);
//  - helmets: the rider's texture also covers the suit, so instead the
//    vertices the game placed with the rider's "Head" frame. Bikes and riders
//    are D3DIMSoultreeObjects; their render (0x441ce0) places each part with
//    0x4a3070(dst, src, frame + 0xf8, n, 32, 32), and frame + 0x38 is the
//    frame's name. We hook that call and remember which frame wrote which
//    vertex range; a draw's vertices are looked up in those ranges.
//
// The reflected picture is a small cube map:
//  - Reflections=1 (sky): the track's own sky box. The game draws the sky
//    as up to 48 textured tiles around the camera, only the ones in view, so
//    each tile is kept the first time it is drawn and the cube is built from
//    all tiles seen so far (the start-of-race fly-round shows most of them).
//    Below the horizon is the fog colour, darkened (ReflectGround).
//  - Reflections=2 (live): the same sky, plus the frame's world draws
//    (terrain, trees, track objects; not bikes or riders) drawn again into
//    the cube from the player's bike, ReflectLiveFaces of the six faces per
//    frame. Only what the game sent this frame can be drawn again, and the
//    game leaves out what is behind the camera, so those directions show sky.
//    The draws are not converted again: they are re-issued from the vertex
//    and index data already on the GPU, with a different camera.
//
// Each reflective draw is drawn a second time on top of itself (depth equal,
// no depth write), adding the cube's colour by a Fresnel factor:
// k = strength * (F0 + (1 - F0) * (1 - cos)^5), so surfaces facing the camera
// reflect a little and edges more. The cube's smaller mip levels make the
// reflection soft (ReflectBlur).
#include "reflect.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "../config.h"
#include "../patch.h"
#include "../xp_compat.h"
#include "backend.h"
#include "device.h"
#include "gpushadow.h"
#include "surface.h"

namespace dd {

using namespace d3d7;

namespace {

// ------------------------------------------------------------------ settings

struct Pattern {
  std::string glob;
  float k;
  int mat = 0;  // material: 0 bike, 1 helmet, 2 rider body
};

// How a material reflects: share of its strength seen head-on, and how soft.
struct Mat {
  float f0;
  float blur;
  float sun;    // sun highlight brightness
  float shine;  // highlight sharpness (Blinn-Phong exponent)
};

struct RCfg {
  bool loaded = false;
  int mode = 0;
  float strength = 1.0f;
  float bike = 1.0f;
  float helmet = 1.0f;
  float rider = 0.25f;
  Mat mats[3] = {{0.25f, 4.0f, 0.6f, 40.0f}, {0.25f, 4.0f, 0.7f, 40.0f}, {0.1f, 4.0f, 0.15f, 8.0f}};
  float sun = 1.0f;  // ReflectSun: multiplies the sun highlights, 0 = none
  bool debug = false;
  bool liquid = false;  // LiquidMetal: bikes and riders all chrome
  int size = 128;
  int live_faces = 1;
  float ground = 1.0f;
  bool log = false;
  // ReflectCache: world draws kept for later frames, so the live cube shows
  // what is around the bike behind the camera too.
  bool cache = true;
  int cache_mb = 32;
  float cache_secs = 20.0f;
  float cache_radius = 0.0f;  // 0: the game's fog end
  std::vector<Pattern> textures;  // texture name -> strength (bikes, plus ReflectTextures)
  std::vector<Pattern> frames;    // frame name -> strength (helmet, plus ReflectFrames)
  std::vector<std::string> exclude;
} g_cfg;

bool Glob(const char* p, const char* s) {
  // '*' any run, '?' any one; case does not matter.
  if (!*p) return !*s;
  if (*p == '*') return Glob(p + 1, s) || (*s && Glob(p, s + 1));
  if (!*s) return false;
  if (*p != '?' && std::tolower(static_cast<unsigned char>(*p)) != std::tolower(static_cast<unsigned char>(*s)))
    return false;
  return Glob(p + 1, s + 1);
}

std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "a*, b:0.3, c" -> patterns (default strength `k`). An inline "; comment"
// ends the list.
void ParseList(const char* text, float k, int mat, std::vector<Pattern>& out) {
  std::string s(text);
  if (size_t semi = s.find(';'); semi != std::string::npos) s.resize(semi);
  size_t at = 0;
  while (at <= s.size()) {
    size_t comma = s.find(',', at);
    std::string item = Trim(s.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
    if (!item.empty()) {
      float v = k;
      if (size_t colon = item.find(':'); colon != std::string::npos) {
        v = static_cast<float>(std::atof(item.c_str() + colon + 1));
        item = Trim(item.substr(0, colon));
      }
      if (!item.empty()) out.push_back({item, v, mat});
    }
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
}

float ReadF(const char* key, float def, const char* ini) {
  char buf[64] = {};
  GetPrivateProfileStringA("mcm2fix", key, "", buf, sizeof(buf), ini);
  if (!buf[0]) return def;
  char* end = nullptr;
  double v = std::strtod(buf, &end);
  return end != buf ? static_cast<float>(v) : def;
}

const RCfg& Cfg() {
  if (g_cfg.loaded) return g_cfg;
  g_cfg.loaded = true;
  char ini[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, ini, MAX_PATH);
  if (char* slash = std::strrchr(ini, '\\')) slash[1] = '\0';
  xp::Append(ini, "mcm2fix.ini");
  g_cfg.mode = std::clamp(static_cast<int>(GetPrivateProfileIntA("mcm2fix", "Reflections", 2, ini)), 0, 2);
  g_cfg.strength = std::clamp(ReadF("ReflectStrength", 1.0f, ini), 0.0f, 4.0f);
  g_cfg.bike = std::clamp(ReadF("ReflectBike", 1.0f, ini), 0.0f, 1.0f);
  g_cfg.helmet = std::clamp(ReadF("ReflectHelmet", 1.0f, ini), 0.0f, 1.0f);
  g_cfg.rider = std::clamp(ReadF("ReflectRider", 0.25f, ini), 0.0f, 1.0f);
  {
    // Per material Fresnel and blur; the older shared keys (ReflectFresnel,
    // ReflectBlur) still set the bike's and helmet's when given.
    const float f0 = ReadF("ReflectFresnel", -1.0f, ini), blur = ReadF("ReflectBlur", -1.0f, ini);
    static const char* const kNames[3] = {"Bike", "Helmet", "Rider"};
    for (int m = 0; m < 3; ++m) {
      Mat& mt = g_cfg.mats[m];
      if (m < 2 && f0 >= 0) mt.f0 = f0;
      if (m < 2 && blur >= 0) mt.blur = blur;
      char key[64];
      std::snprintf(key, sizeof(key), "Reflect%sFresnel", kNames[m]);
      mt.f0 = std::clamp(ReadF(key, mt.f0, ini), 0.0f, 1.0f);
      std::snprintf(key, sizeof(key), "Reflect%sBlur", kNames[m]);
      mt.blur = std::clamp(ReadF(key, mt.blur, ini), 0.0f, 8.0f);
      std::snprintf(key, sizeof(key), "Reflect%sSun", kNames[m]);
      mt.sun = std::clamp(ReadF(key, mt.sun, ini), 0.0f, 4.0f);
      std::snprintf(key, sizeof(key), "Reflect%sShine", kNames[m]);
      mt.shine = std::clamp(ReadF(key, mt.shine, ini), 1.0f, 2000.0f);
    }
  }
  g_cfg.sun = std::clamp(ReadF("ReflectSun", 1.0f, ini), 0.0f, 4.0f);
  g_cfg.cache = GetPrivateProfileIntA("mcm2fix", "ReflectCache", 1, ini) != 0;
  g_cfg.cache_mb = std::clamp(static_cast<int>(GetPrivateProfileIntA("mcm2fix", "ReflectCacheMB", 32, ini)), 4, 512);
  g_cfg.cache_secs = std::clamp(ReadF("ReflectCacheSecs", 20.0f, ini), 0.5f, 600.0f);
  g_cfg.cache_radius = std::clamp(ReadF("ReflectCacheRadius", 0.0f, ini), 0.0f, 1e6f);
  int size = static_cast<int>(GetPrivateProfileIntA("mcm2fix", "ReflectSize", 128, ini));
  int p = 16;
  while (p < size && p < 1024) p <<= 1;
  g_cfg.size = p;
  g_cfg.live_faces = std::clamp(static_cast<int>(GetPrivateProfileIntA("mcm2fix", "ReflectLiveFaces", 1, ini)), 1, 6);
  g_cfg.ground = std::clamp(ReadF("ReflectGround", 1.0f, ini), 0.0f, 2.0f);
  g_cfg.log = GetPrivateProfileIntA("mcm2fix", "ReflectLog", 0, ini) != 0;
  g_cfg.debug = GetPrivateProfileIntA("mcm2fix", "ReflectDebug", 0, ini) != 0;
  g_cfg.liquid = GetPrivateProfileIntA("mcm2fix", "LiquidMetal", 0, ini) != 0;
  if (g_cfg.liquid && !g_cfg.mode) g_cfg.mode = 2;
  char buf[1024] = {};
  GetPrivateProfileStringA("mcm2fix", "ReflectBikeTextures",
                           "honda*, ktm*, yamaha*, kazuyuki*, suzuki*, kawasaki*, husq*", buf, sizeof(buf), ini);
  ParseList(buf, g_cfg.bike, 0, g_cfg.textures);
  GetPrivateProfileStringA("mcm2fix", "ReflectTextures", "", buf, sizeof(buf), ini);
  ParseList(buf, g_cfg.bike, 0, g_cfg.textures);
  GetPrivateProfileStringA("mcm2fix", "ReflectHelmetFrames", "Head", buf, sizeof(buf), ini);
  ParseList(buf, g_cfg.helmet, 1, g_cfg.frames);
  GetPrivateProfileStringA("mcm2fix", "ReflectRiderFrames",
                           "PELVIS, LWRTORSO, UPRTORSO, UPRLEG*, LWRLEG*, UPRARM*, LWRARM*, HANDL, HANDR, FOOTL, FOOTR",
                           buf, sizeof(buf), ini);
  ParseList(buf, g_cfg.rider, 2, g_cfg.frames);
  GetPrivateProfileStringA("mcm2fix", "ReflectFrames", "", buf, sizeof(buf), ini);
  ParseList(buf, g_cfg.helmet, 1, g_cfg.frames);
  std::vector<Pattern> ex;
  GetPrivateProfileStringA("mcm2fix", "ReflectExclude", "tire*", buf, sizeof(buf), ini);
  ParseList(buf, 0, 0, ex);
  for (auto& e : ex) g_cfg.exclude.push_back(e.glob);
  if (g_cfg.mode) {
    std::string t, f;
    for (auto& x : g_cfg.textures) t += x.glob + ":" + std::to_string(x.k).substr(0, 4) + " ";
    for (auto& x : g_cfg.frames) f += x.glob + ":" + std::to_string(x.k).substr(0, 4) + " ";
    if (g_cfg.liquid) Log("reflections: LiquidMetal on: bikes and riders are drawn in chrome");
    Log("reflections: mode %d (%s), strength %.2f, F0/blur bike %.2f/%.1f helmet %.2f/%.1f rider %.2f/%.1f, cube %dpx, "
        "live faces/frame %d, ground %.2f; textures [%s] frames [%s]",
        g_cfg.mode, g_cfg.mode == 1 ? "sky" : "live", g_cfg.strength, g_cfg.mats[0].f0, g_cfg.mats[0].blur,
        g_cfg.mats[1].f0, g_cfg.mats[1].blur, g_cfg.mats[2].f0, g_cfg.mats[2].blur, g_cfg.size, g_cfg.live_faces,
        g_cfg.ground, t.c_str(), f.c_str());
    if (g_cfg.mode == 2)
      Log("reflections: draw cache %s, %d MB, kept %.1f s, radius %s", g_cfg.cache ? "on" : "off", g_cfg.cache_mb,
          g_cfg.cache_secs, g_cfg.cache_radius > 0 ? std::to_string(g_cfg.cache_radius).c_str() : "fog end");
    Log("reflections: sun highlight x%.2f, brightness/sharpness bike %.2f/%.0f helmet %.2f/%.0f rider %.2f/%.0f",
        g_cfg.sun, g_cfg.mats[0].sun, g_cfg.mats[0].shine, g_cfg.mats[1].sun, g_cfg.mats[1].shine, g_cfg.mats[2].sun,
        g_cfg.mats[2].shine);
  }
  return g_cfg;
}

// ------------------------------------------------------------------ hooks

const void* g_cur_obj = nullptr;

struct FrameRange {
  uintptr_t end;
  float k;    // strength from the frame lists, < 0 = not listed
  int mat;
  int name;   // index into g_frame_names
};
std::map<uintptr_t, FrameRange> g_ranges;  // by start address
std::vector<std::string> g_frame_names;
std::vector<bool> g_frame_rider;  // a bike or rider part (LiquidMetal)
std::unordered_map<const void*, bool> g_obj_rider;  // objects found to be bikes or riders
bool g_liquid_now = false;  // this draw is a LiquidMetal one
std::unordered_map<std::string, int> g_frame_ids;
unsigned g_xforms = 0, g_obj_draws = 0;

int FrameNameId(const void* frame) {
  char name[33] = {};
  MEMORY_BASIC_INFORMATION mbi{};
  const char* src = static_cast<const char*>(frame) + 0x38;
  if (VirtualQuery(src, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
      reinterpret_cast<uintptr_t>(src) + 32 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
    std::memcpy(name, src, 32);
  name[32] = 0;
  for (char* c = name; *c; ++c)
    if (static_cast<unsigned char>(*c) < 32 || static_cast<unsigned char>(*c) > 126) {
      *c = 0;
      break;
    }
  std::string s(name[0] ? name : "?");
  auto it = g_frame_ids.find(s);
  if (it != g_frame_ids.end()) return it->second;
  const int id = static_cast<int>(g_frame_names.size());
  g_frame_names.push_back(s);
  g_frame_ids[s] = id;
  {
    static const char* const kParts[] = {"BIKE", "HANDLEBARS", "SWINGARM", "SHOCKS", "TIRE*", "KICKSTART", "PELVIS",
                                         "*TORSO", "*LEG*", "*ARM*", "HEAD", "HAND*", "FOOT*"};
    bool r = false;
    for (const char* k : kParts) r = r || Glob(k, s.c_str());
    g_frame_rider.push_back(r);
  }
  if (Cfg().log && id < 200) Log("reflections: part frame '%s' (frame %08x)", s.c_str(), static_cast<unsigned>(reinterpret_cast<uintptr_t>(frame)));
  return id;
}

const Pattern* FramePattern(int id) {
  const std::string& n = g_frame_names[id];
  for (const Pattern& p : Cfg().frames)
    if (Glob(p.glob.c_str(), n.c_str())) return &p;
  return nullptr;
}

using XformFn = void(__cdecl*)(void*, const void*, const float*, int, int, int);
XformFn g_xform_next = nullptr;
void __cdecl XformHook(void* dst, const void* src, const float* m, int n, int ds, int ss) {
  ++g_xforms;
  if (dst && m && n > 0 && ds > 0) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(dst);
    const uintptr_t e = a + static_cast<uintptr_t>(n) * static_cast<uintptr_t>(ds);
    auto it = g_ranges.find(a);
    if (it == g_ranges.end() || it->second.end != e) {
      // Drop ranges this one overlaps (memory reused), then add it.
      auto lo = g_ranges.lower_bound(a);
      if (lo != g_ranges.begin() && std::prev(lo)->second.end > a) --lo;
      while (lo != g_ranges.end() && lo->first < e) lo = g_ranges.erase(lo);
      const int id = FrameNameId(reinterpret_cast<const uint8_t*>(m) - 0xf8);
      const Pattern* fp = FramePattern(id);
      g_ranges[a] = FrameRange{e, fp ? fp->k : -1.0f, fp ? fp->mat : 0, id};
      if (g_ranges.size() > 20000) g_ranges.clear();
    }
  }
  g_xform_next(dst, src, m, n, ds, ss);
}

using ObjDrawFn = void(__fastcall*)(void*, void*);
ObjDrawFn g_obj_next = nullptr;
void __fastcall ObjDrawHook(void* self, void* edx) {
  ++g_obj_draws;
  const void* was = g_cur_obj;
  g_cur_obj = self;
  g_obj_next(self, edx);
  g_cur_obj = was;
}

uintptr_t CallTarget(uintptr_t site) {
  int32_t rel;
  std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
  return site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

// ------------------------------------------------------------------ maths

void Mul(const float* a, const float* b, float* out) {
  float r[16];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
  std::memcpy(out, r, sizeof(r));
}

void CameraPos(const D3DMATRIX& v, float out[3]) {
  // view = [R 0; t 1] (row vectors): camera = -t R^T.
  for (int j = 0; j < 3; ++j) out[j] = -(v.m[3][0] * v.m[j][0] + v.m[3][1] * v.m[j][1] + v.m[3][2] * v.m[j][2]);
}

void Xform(const float* p, const D3DMATRIX& m, float out[3]) {
  for (int j = 0; j < 3; ++j) out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
}

// Standard Direct3D cube face cameras (+X -X +Y -Y +Z -Z), at `eye`.
void FaceView(int f, const float eye[3], float out[16]) {
  static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static const float ups[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
  const float* z = dirs[f];
  const float* u = ups[f];
  float x[3] = {u[1] * z[2] - u[2] * z[1], u[2] * z[0] - u[0] * z[2], u[0] * z[1] - u[1] * z[0]};
  float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  float m[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                 -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]),
                 -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
                 -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1};
  std::memcpy(out, m, sizeof(m));
}

void FaceProj(float n, float f, float out[16]) {
  const float q = f / (f - n);
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, q, 1, 0, 0, -n * q, 0};
  std::memcpy(out, m, sizeof(m));
}

double Now() {
  static LARGE_INTEGER f{};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// ------------------------------------------------------------------ GPU side

const char kShader[] = R"(
cbuffer P : register(b2) {
  row_major float4x4 wvp;
  row_major float4x4 world;
  float4 vp;      // viewport x y w h
  float4 cam;     // camera position (world)
  float4 fog;     // start end density mode
  float4 params;  // strength, -, full (1 = no Fresnel, full strength), alpha cut
  float4 sun;     // towards the sun (unit), w brightness (0 = no sun)
};
TextureCube env : register(t3);
SamplerState envsmp : register(s3);
Texture2D tex0 : register(t0);
SamplerState smp0 : register(s0);
struct VIn { float3 pos : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; float k : TEXCOORD1; float4 m : TEXCOORD2; };
struct VOut {
  float4 pos : SV_Position;
  float3 wpos : TEXCOORD0;
  float3 n : TEXCOORD1;
  float2 uv : TEXCOORD2;
  float k : TEXCOORD3;
  float fogw : TEXCOORD4;
  float4 m : TEXCOORD5;  // material: F0, blur level, sun brightness, sharpness
};
VOut vs_main(VIn i) {
  VOut o;
  float4 p = mul(float4(i.pos, 1), wvp);
  p.x += p.w / vp.z;
  p.y -= p.w / vp.w;
  o.pos = p;
  o.fogw = p.w;
  o.wpos = mul(float4(i.pos, 1), world).xyz;
  o.n = mul(i.n, (float3x3)world);
  o.uv = i.uv;
  o.k = i.k;
  o.m = i.m;
  return o;
}
float4 ps_main(VOut i) : SV_Target {
  if (params.w > 0 && tex0.Sample(smp0, i.uv).a < params.w) discard;
  float3 n = normalize(i.n);
  float3 v = normalize(i.wpos - cam.xyz);
  if (dot(n, v) > 0) n = -n;
  float3 r = reflect(v, n);
  float3 e = env.SampleLevel(envsmp, r, params.z >= 1 ? 0.5 : i.m.y).rgb;
  float c = 1 - saturate(dot(-v, n));
  float c2 = c * c;
  float f0 = params.z >= 1 ? 1 : i.m.x;
  float k = (params.z >= 1 ? (i.k > 0 ? 1 : 0) : i.k) * params.x * (f0 + (1 - f0) * c2 * c2 * c);
  float f = 1;
  if (fog.w != 0) {
    float d = i.fogw;
    if (fog.w == 3) f = (fog.y - d) / max(fog.y - fog.x, 1e-6);
    else if (fog.w == 1) f = exp(-fog.z * d);
    else f = exp(-(fog.z * d) * (fog.z * d));
    f = saturate(f);
  }
  k = saturate(k * f);
  // Sun highlight (Blinn-Phong), only on the sunny side.
  float s = 0;
  if (sun.w > 0 && i.m.z > 0) {
    float3 h = normalize(sun.xyz - v);
    float nl = dot(n, sun.xyz);
    s = sun.w * i.m.z * pow(saturate(dot(n, h)), i.m.w) * saturate(nl * 4) * f;
  }
  return float4(e * k + s * float3(1, 0.97, 0.9), k);
}
// Sky tiles into the cube.
cbuffer S : register(b3) { row_major float4x4 svp; };
Texture2D skytex : register(t5);
SamplerState skysmp : register(s5);
struct SIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct SOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
SOut vs_sky(SIn i) { SOut o; o.pos = mul(float4(i.pos, 1), svp); o.uv = i.uv; return o; }
float4 ps_sky(SOut i) : SV_Target { return float4(skytex.Sample(skysmp, i.uv).rgb, 1); }
)";

struct PassConstants {
  float wvp[16];
  float world[16];
  float vp[4];
  float cam[4];
  float fog[4];
  float params[4];
  float sun[4];  // towards the sun (world, unit), w = ReflectSun (0: no sun)
};

// The device's main vertex shader constants (device.cpp VSConstants).
struct GameVSConstants {
  float wvp[16];
  float vp[4];
  float mode[4];
};

struct PassVertex {
  float pos[3];
  float n[3];
  float uv[2];
  float k;
  float f0, blur, sun, shine;
};
static_assert(sizeof(PassVertex) == 52, "pass vertex");

struct SkyVertex {
  float pos[3];
  float uv[2];
};

struct Gpu {
  ID3D11Device* device = nullptr;  // what these were made on
  bool failed = false;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* layout = nullptr;
  ID3D11VertexShader* sky_vs = nullptr;
  ID3D11PixelShader* sky_ps = nullptr;
  ID3D11InputLayout* sky_layout = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11Buffer* sky_cb = nullptr;
  ID3D11Buffer* game_vs_cb = nullptr;  // our own copy for replays (b0)
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT vb_size = 1u << 20, ib_size = 1u << 19, vb_pos = 0, ib_pos = 0;
  ID3D11BlendState* blend = nullptr;      // src alpha, inv src alpha, colour only
  ID3D11BlendState* opaque = nullptr;
  ID3D11DepthStencilState* depth_eq = nullptr;  // test <=, no write
  ID3D11DepthStencilState* depth_off = nullptr;
  ID3D11RasterizerState* raster[3] = {};  // pass: none, front, back culled; small bias toward the camera
  ID3D11RasterizerState* raster_none = nullptr;  // cube drawing
  ID3D11SamplerState* env_smp = nullptr;
  ID3D11SamplerState* clamp_smp = nullptr;
  // The cube.
  int size = 0;
  ID3D11Texture2D* cube = nullptr;
  ID3D11ShaderResourceView* cube_srv = nullptr;
  ID3D11RenderTargetView* face_rtv[6] = {};
  ID3D11Texture2D* zt = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  bool cube_valid = false;  // drawn at least once
  // ReflectCache: copies of world draws (the device's vertex layout).
  ID3D11Buffer* cvb = nullptr;
  ID3D11Buffer* cib = nullptr;
  UINT cvb_size = 0, cib_size = 0, cvb_pos = 0, cib_pos = 0;
  bool cache_failed = false;
} g;

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

void FreeGpu() {
  Rel(g.vs), Rel(g.ps), Rel(g.layout), Rel(g.sky_vs), Rel(g.sky_ps), Rel(g.sky_layout);
  Rel(g.cb), Rel(g.sky_cb), Rel(g.game_vs_cb), Rel(g.vb), Rel(g.ib);
  Rel(g.blend), Rel(g.opaque), Rel(g.depth_eq), Rel(g.depth_off), Rel(g.raster_none);
  for (auto*& r : g.raster) Rel(r);
  Rel(g.env_smp), Rel(g.clamp_smp);
  Rel(g.cube_srv), Rel(g.cube), Rel(g.zt), Rel(g.dsv);
  for (auto*& r : g.face_rtv) Rel(r);
  Rel(g.cvb), Rel(g.cib);
  g.cvb_size = g.cib_size = g.cvb_pos = g.cib_pos = 0;
  g.cache_failed = false;
  g.cube_valid = false;
  g.size = 0;
}

struct SkyTile {
  ID3D11ShaderResourceView* srv = nullptr;
  std::vector<SkyVertex> v;
  std::vector<uint16_t> idx;
};
std::vector<SkyTile> g_sky;
std::string g_sky_name;  // which sky the tiles are from
bool g_sky_dirty = false;
unsigned g_sky_logged = 0;

void ClearSky() {
  for (auto& t : g_sky) Rel(t.srv);
  g_sky.clear();
  g_sky_dirty = true;
}

// ReflectCache: a world draw kept on the GPU (copied out of the device's
// vertex and index buffers, which are reused every frame), with its state
// and world-space bounds.
struct CacheEntry {
  ReflectReplay st;  // state; srv AddRef'd
  UINT vbyte = 0, vbytes = 0;  // in g.cvb
  UINT ibyte = 0, ibytes = 0;  // in g.cib (0 bytes: not indexed)
  INT vstart = 0;   // the indices' own first vertex (subtracted when drawn)
  UINT count = 0, nidx = 0;
  float lo[3], hi[3];
  double seen = 0;
  unsigned seen_frame = 0;
  unsigned serial = 0;
};
std::unordered_map<uint64_t, CacheEntry> g_cache;
// Entries in the order their room was taken (both rings fill in this order,
// so the oldest are in the way first): key and serial.
std::deque<std::pair<uint64_t, unsigned>> g_cache_order;
unsigned g_cache_serial = 0;
std::vector<const CacheEntry*> g_cache_draw;  // this cube update's
float g_cache_radius_used = 0;
unsigned g_cache_frame = 1;  // frames recorded (the cache's own count)
struct CStats {
  unsigned hits = 0, inserts = 0, evicted = 0, aged = 0, too_big = 0, drawn = 0, culled_view = 0, culled_far = 0,
           culled_old = 0;
  double copied = 0;  // bytes
} g_cs;

void ReleaseEntry(CacheEntry& e) {
  for (auto*& s : e.st.srv) Rel(s);
}

void ClearCache() {
  for (auto& kv : g_cache) ReleaseEntry(kv.second);
  g_cache.clear();
  g_cache_order.clear();
  g.cvb_pos = g.cib_pos = 0;
}

// Room for an entry's vertices and indices in the two ring buffers (each
// wraps to its start when full); the oldest entries in the way go.
void CacheAlloc(UINT vbytes, UINT ibytes, UINT stride, UINT* vat, UINT* iat) {
  const UINT vpos = (g.cvb_pos + stride - 1) / stride * stride, ipos = (g.cib_pos + 3) / 4 * 4;
  const bool vwrap = vpos + vbytes > g.cvb_size, iwrap = ibytes && ipos + ibytes > g.cib_size;
  *vat = vwrap ? 0 : vpos;
  *iat = !ibytes ? 0 : iwrap ? 0 : ipos;
  g.cvb_pos = *vat + vbytes;
  if (ibytes) g.cib_pos = *iat + ibytes;
  while (!g_cache_order.empty()) {
    const auto f = g_cache_order.front();
    auto it = g_cache.find(f.first);
    if (it == g_cache.end() || it->second.serial != f.second) {
      g_cache_order.pop_front();  // already gone
      continue;
    }
    const CacheEntry& e = it->second;
    const bool in_v = e.vbyte < *vat + vbytes && *vat < e.vbyte + e.vbytes;
    const bool in_i = ibytes && e.ibytes && e.ibyte < *iat + ibytes && *iat < e.ibyte + e.ibytes;
    // After a wrap, what is left at the ring's end is older than anything at
    // its start: it goes first.
    const bool tail = (vwrap && e.vbyte >= *vat + vbytes) || (iwrap && e.ibytes && e.ibyte >= *iat + ibytes);
    if (!in_v && !in_i && !tail) break;
    ReleaseEntry(it->second);
    g_cache.erase(it);
    g_cache_order.pop_front();
    ++g_cs.evicted;
  }
}

// Draw cache key: positions rounded to this (world units).
constexpr float kCacheGrid = 8.0f;

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

// Is the box entirely outside the view (row-vector matrix vp)?
bool BoxOutside(const float lo[3], const float hi[3], const float* vp) {
  int out[6] = {};
  for (int c = 0; c < 8; ++c) {
    const float p[3] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]};
    float q[4];
    for (int k = 0; k < 4; ++k) q[k] = p[0] * vp[k] + p[1] * vp[4 + k] + p[2] * vp[8 + k] + vp[12 + k];
    out[0] += q[0] < -q[3];
    out[1] += q[0] > q[3];
    out[2] += q[1] < -q[3];
    out[3] += q[1] > q[3];
    out[4] += q[2] < 0;
    out[5] += q[2] > q[3];
  }
  for (int k : out)
    if (k == 8) return true;
  return false;
}

float BoxDist2(const float lo[3], const float hi[3], const float p[3]) {
  float d = 0;
  for (int k = 0; k < 3; ++k) {
    const float e = p[k] < lo[k] ? lo[k] - p[k] : p[k] > hi[k] ? p[k] - hi[k] : 0.0f;
    d += e * e;
  }
  return d;
}

bool MakeGpu() {
  Backend& b = B();
  if (g.device == b.device && g.vs) return true;
  if (g.device == b.device && g.failed) return false;
  if (g.device != b.device) {
    ClearCache();
    FreeGpu();
    ClearSky();
    g.failed = false;
  }
  g.device = b.device;
  g.failed = true;
  ID3D11Device* dev = b.device;
  ID3DBlob* vs = Compile(kShader, "vs_main", "vs_4_0");
  ID3DBlob* ps = Compile(kShader, "ps_main", "ps_4_0");
  ID3DBlob* svs = Compile(kShader, "vs_sky", "vs_4_0");
  ID3DBlob* sps = Compile(kShader, "ps_sky", "ps_4_0");
  bool ok = vs && ps && svs && sps;
  if (ok) {
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.ps);
    dev->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, &g.sky_vs);
    dev->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, &g.sky_ps);
    const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(el, 5, vs->GetBufferPointer(), vs->GetBufferSize(), &g.layout);
    const D3D11_INPUT_ELEMENT_DESC sel[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(sel, 2, svs->GetBufferPointer(), svs->GetBufferSize(), &g.sky_layout);
  }
  Rel(vs), Rel(ps), Rel(svs), Rel(sps);
  if (!ok || !g.layout || !g.sky_layout) {
    Log("reflections: shaders failed; reflections off");
    return false;
  }
  D3D11_BUFFER_DESC bd{};
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.ByteWidth = sizeof(PassConstants);
  dev->CreateBuffer(&bd, nullptr, &g.cb);
  bd.ByteWidth = 64;
  dev->CreateBuffer(&bd, nullptr, &g.sky_cb);
  bd.ByteWidth = sizeof(GameVSConstants);
  dev->CreateBuffer(&bd, nullptr, &g.game_vs_cb);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.ByteWidth = g.vb_size;
  dev->CreateBuffer(&bd, nullptr, &g.vb);
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  bd.ByteWidth = g.ib_size;
  dev->CreateBuffer(&bd, nullptr, &g.ib);

  D3D11_BLEND_DESC bl{};
  bl.RenderTarget[0].BlendEnable = TRUE;
  // Premultiplied: the reflection by its strength, plus the sun highlight.
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                             D3D11_COLOR_WRITE_ENABLE_BLUE;
  dev->CreateBlendState(&bl, &g.blend);
  D3D11_BLEND_DESC op{};
  op.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&op, &g.opaque);
  D3D11_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  dev->CreateDepthStencilState(&ds, &g.depth_eq);
  ds.DepthEnable = FALSE;
  dev->CreateDepthStencilState(&ds, &g.depth_off);
  for (int c = 0; c < 3; ++c) {
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = c == 0 ? D3D11_CULL_NONE : c == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
    r.DepthBias = -64;
    r.SlopeScaledDepthBias = -2.0f;
    r.DepthClipEnable = TRUE;
    r.MultisampleEnable = TRUE;
    dev->CreateRasterizerState(&r, &g.raster[c]);
  }
  D3D11_RASTERIZER_DESC rn{};
  rn.FillMode = D3D11_FILL_SOLID;
  rn.CullMode = D3D11_CULL_NONE;
  rn.DepthClipEnable = TRUE;
  dev->CreateRasterizerState(&rn, &g.raster_none);
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&sd, &g.env_smp);
  dev->CreateSamplerState(&sd, &g.clamp_smp);

  const int size = Cfg().size;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = static_cast<UINT>(size);
  td.MipLevels = 0;
  td.ArraySize = 6;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.cube))) {
    Log("reflections: cube texture failed; reflections off");
    return false;
  }
  dev->CreateShaderResourceView(g.cube, nullptr, &g.cube_srv);
  for (int f = 0; f < 6; ++f) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.MipSlice = 0;
    rd.Texture2DArray.FirstArraySlice = static_cast<UINT>(f);
    rd.Texture2DArray.ArraySize = 1;
    dev->CreateRenderTargetView(g.cube, &rd, &g.face_rtv[f]);
  }
  D3D11_TEXTURE2D_DESC zd{};
  zd.Width = zd.Height = static_cast<UINT>(size);
  zd.MipLevels = 1;
  zd.ArraySize = 1;
  zd.Format = DXGI_FORMAT_D32_FLOAT;
  zd.SampleDesc.Count = 1;
  zd.Usage = D3D11_USAGE_DEFAULT;
  zd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  dev->CreateTexture2D(&zd, nullptr, &g.zt);
  if (g.zt) dev->CreateDepthStencilView(g.zt, nullptr, &g.dsv);
  g.size = size;
  g.failed = !(g.cb && g.vb && g.ib && g.blend && g.depth_eq && g.cube_srv && g.face_rtv[5] && g.dsv && g.env_smp);
  if (g.failed) Log("reflections: GPU resources failed; reflections off");
  else Log("reflections: GPU resources made (cube %dx%d x6, mipmapped)", size, size);
  return !g.failed;
}

// Map room in one of our dynamic buffers.
template <class Fill>
UINT Push(ID3D11Buffer* buf, UINT size, UINT& pos, UINT bytes, UINT align, Fill fill) {
  if (bytes > size) return UINT(-1);
  D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
  UINT at = (pos + align - 1) / align * align;  // a whole number of vertices in
  if (at + bytes > size) {
    at = 0;
    map = D3D11_MAP_WRITE_DISCARD;
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(B().ctx->Map(buf, 0, map, 0, &m))) return UINT(-1);
  fill(static_cast<uint8_t*>(m.pData) + at);
  B().ctx->Unmap(buf, 0);
  pos = at + bytes;
  return at;
}

// ------------------------------------------------------------------ per draw

struct TexInfo {
  std::string name;
  float k = 0;
  int mat = 0;
  bool excluded = false;
};
std::unordered_map<const Surface*, TexInfo> g_tex;

const TexInfo& TexFor(Surface* t) {
  static TexInfo none;
  if (!t) return none;
  TexInfo& ti = g_tex[t];
  if (ti.name != t->TextureName()) {
    ti.name = t->TextureName();
    ti.k = 0;
    ti.excluded = false;
    for (const std::string& e : Cfg().exclude)
      if (Glob(e.c_str(), ti.name.c_str())) ti.excluded = true;
    if (!ti.excluded)
      for (const Pattern& p : Cfg().textures)
        if (Glob(p.glob.c_str(), ti.name.c_str())) {
          ti.k = p.k;
          ti.mat = p.mat;
          break;
        }
  }
  return ti;
}

// Welding: which vertices of a draw share a place. Kept per draw (same
// vertex array, count and indices), since the game redraws the same meshes.
struct Weld {
  std::vector<int> group;  // per vertex
  int groups = 0;
  unsigned used = 0;
};
std::unordered_map<uint64_t, Weld> g_welds;

uint64_t WeldKey(const void* verts, DWORD count, const void* idx, DWORD nidx, DWORD prim) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  mix(reinterpret_cast<uintptr_t>(verts));
  mix(count);
  mix(reinterpret_cast<uintptr_t>(idx));
  mix(nidx);
  mix(prim);
  return h;
}

// RStats (ReflectLog).
struct RStats {
  unsigned faces_skipped = 0, frames = 0, passes = 0, tris = 0, skipped_no_tris = 0, faces = 0, replays = 0, replay_stale = 0,
           sky_tiles_drawn = 0, cube_builds = 0, records = 0, sun_draws = 0;
  double pass_s = 0, cube_s = 0, record_s = 0;
  unsigned capture_bike = 0, capture_cam = 0;
} g_st;
double g_st_since = 0;
float g_logged_sun[3] = {};  // ReflectLog: the sun direction last logged

std::vector<ReflectReplay> g_replays;
int g_face_cursor = 0;
unsigned g_frame = 0;
bool g_saw_3d = false;
// Where the cube is drawn from: the reflective draw nearest the camera last
// frame (the player's bike), else the camera.
// Per object this frame: sum of its reflective vertices (world).
struct ObjAcc {
  float sum[3];
  float n;
};
std::unordered_map<const void*, ObjAcc> g_obj_acc;
float g_last_eye[3] = {};
bool g_have_eye = false;

std::vector<PassVertex> g_pv;
std::vector<uint16_t> g_pi;
std::vector<float> g_acc;  // per weld group normal sums
std::vector<std::string> g_logged_pairs;

void LogObjectPair(const std::string& tex, const std::string& frame, DWORD verts, float k) {
  if (!Cfg().log || g_logged_pairs.size() > 400) return;
  std::string key = tex + "|" + frame;
  for (const auto& s : g_logged_pairs)
    if (s == key) return;
  g_logged_pairs.push_back(key);
  Log("reflections: bike/rider draw texture '%s' frame '%s' (%lu verts) strength %.2f", tex.c_str(), frame.c_str(),
      static_cast<unsigned long>(verts), k);
}

}  // namespace

// ------------------------------------------------------------------ public

int ReflectMode() { return Cfg().mode; }

bool IsSkyTextureName(const std::string& n) {
  const char* s = n.c_str();
  size_t pre = 0;
  if (!std::strncmp(s, "terra", 5)) pre = 5;
  else if (!std::strncmp(s, "burn", 4)) pre = 4;
  else if (!std::strncmp(s, "wind", 4)) pre = 4;
  else return false;
  return n.size() > pre + 3 && std::isdigit(static_cast<unsigned char>(s[pre])) &&
         std::isdigit(static_cast<unsigned char>(s[pre + 1])) && s[pre + 2] == '_';
}

const void* ReflectCurrentObject() { return g_cur_obj; }

void ReflectInstallHooks() {
  static bool done = false;
  if (done || !Cfg().mode) return;
  done = true;
  // mov ecx,edi; call (object draw) and push ebp; call (part transform).
  // The calls may already lead to SimSmooth's hooks; the bytes before them
  // must be the game's.
  static const uint8_t draw_pre[] = {0x8b, 0xcf, 0xe8};
  static const uint8_t xform_pre[] = {0x55, 0xe8};
  if (!patch::Matches(0x444FE8, draw_pre, sizeof(draw_pre)) || !patch::Matches(0x441E3E, xform_pre, sizeof(xform_pre))) {
    Log("reflections: game hooks NOT installed (unexpected bytes); helmets and live cube object filter off");
    return;
  }
  // Chain to whatever is there (SimSmooth's part hooks, or the game).
  g_obj_next = reinterpret_cast<ObjDrawFn>(CallTarget(0x444FEA));
  g_xform_next = reinterpret_cast<XformFn>(CallTarget(0x441E3F));
  patch::Call(0x444FEA, reinterpret_cast<void*>(&ObjDrawHook), 5);
  patch::Call(0x441E3F, reinterpret_cast<void*>(&XformHook), 5);
  Log("reflections: hooked object draw (next %08x) and part transform (next %08x)",
      static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_obj_next)),
      static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_xform_next)));
}

// A 3D draw arrived (before batching). Sky tiles are kept for the cube.
// True when this draw gets a reflection pass.
bool Device::ReflectWants(const Draw& d) {
  g_liquid_now = false;
  if (!Cfg().mode || IsD3D9() || !d.verts || d.layout.tl) return false;
  Surface* t = tex_[0];
  if (Cfg().liquid && g_cur_obj) {
    // LiquidMetal: every draw of an object that has bike or rider parts.
    bool& rider = g_obj_rider[g_cur_obj];
    if (!rider && !g_ranges.empty()) {
      const uint8_t* gv = d.game_verts ? d.game_verts : d.verts;
      const uintptr_t a = reinterpret_cast<uintptr_t>(gv) + static_cast<uintptr_t>(d.first) * d.layout.stride;
      const uintptr_t e = a + static_cast<uintptr_t>(d.count) * d.layout.stride;
      auto it = g_ranges.upper_bound(a);
      if (it != g_ranges.begin()) --it;
      for (; it != g_ranges.end() && it->first < e; ++it)
        if (it->second.end > a && g_frame_rider[it->second.name]) rider = true;
    }
    if (g_obj_rider.size() > 4096) g_obj_rider.clear();
    if (rider) {
      g_liquid_now = true;
      return true;
    }
  }
  if (!t) return false;
  const std::string& name = t->TextureName();
  if (!g_cur_obj) {
    if (!name.empty() && IsSkyTextureName(name)) ReflectSky(d);
    return false;
  }
  const TexInfo& ti = TexFor(t);
  if (ti.excluded) {
    LogObjectPair(name, "(excluded texture)", d.count, 0);
    return false;
  }
  if (ti.k > 0) {
    LogObjectPair(name, "(reflective texture)", d.count, ti.k);
    return true;
  }
  // Not a reflective texture: helmet frames only.
  if (g_ranges.empty()) return false;
  const uint8_t* gv = d.game_verts ? d.game_verts : d.verts;
  const uintptr_t a = reinterpret_cast<uintptr_t>(gv) + static_cast<uintptr_t>(d.first) * d.layout.stride;
  const uintptr_t e = a + static_cast<uintptr_t>(d.count) * d.layout.stride;
  auto it = g_ranges.upper_bound(a);
  if (it != g_ranges.begin()) --it;
  if (Cfg().log && (it == g_ranges.end() || (it->second.end <= a && std::next(it) != g_ranges.end() && std::next(it)->first >= e)))
    LogObjectPair(name, "(no part ranges)", d.count, ti.k);
  for (; it != g_ranges.end() && it->first < e; ++it) {
    if (it->second.end <= a) continue;
    if (Cfg().log) {
      LogObjectPair(name, g_frame_names[it->second.name], static_cast<DWORD>((std::min(e, it->second.end) - std::max(a, it->first)) / d.layout.stride),
                    it->second.k > 0 ? it->second.k : ti.k);
    }
    if (it->second.k > 0) return true;
  }
  return false;
}

void Device::ReflectSky(const Draw& d) {
  // Keep this tile (once): camera-relative world positions and texture
  // coordinates, as a triangle list.
  Surface* t = tex_[0];
  const std::string& name = t->TextureName();
  const std::string sky = name.substr(0, name.find('_'));
  if (sky != g_sky_name) {
    if (Cfg().log) Log("reflections: sky '%s' (was '%s'); tiles dropped", sky.c_str(), g_sky_name.c_str());
    ClearSky();
    g_sky_name = sky;
  }
  const GpuTexture* gt = t->SampleTex();
  ID3D11ShaderResourceView* srv = gt ? gt->srv : nullptr;
  if (!srv || d.layout.ntex < 1) return;
  for (const SkyTile& s : g_sky)
    if (s.srv == srv) return;
  if (!MakeGpu()) return;
  float cam[3];
  CameraPos(view_, cam);
  SkyTile tile;
  tile.srv = srv;
  srv->AddRef();
  const size_t stride = d.layout.stride;
  const uint8_t* base = d.verts + static_cast<size_t>(d.first) * stride;
  const int uvo = d.layout.tex[0];
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (DWORD i = 0; i < d.count; ++i) {
    const float* p = reinterpret_cast<const float*>(base + i * stride);
    const float* uv = reinterpret_cast<const float*>(base + i * stride + uvo);
    SkyVertex v;
    float w[3];
    Xform(p, world_, w);
    for (int j = 0; j < 3; ++j) {
      v.pos[j] = w[j] - cam[j];
      lo[j] = std::min(lo[j], v.pos[j]);
      hi[j] = std::max(hi[j], v.pos[j]);
    }
    v.uv[0] = uv[0];
    v.uv[1] = uv[1];
    tile.v.push_back(v);
  }
  const DWORD n = d.idx ? d.nidx : d.count;
  auto at = [&](DWORD i) -> uint16_t { return static_cast<uint16_t>(d.idx ? d.idx[i] : i); };
  if (d.prim == D3DPT_TRIANGLELIST) {
    for (DWORD i = 0; i + 2 < n + 0 && i + 2 < n; i += 3)
      tile.idx.insert(tile.idx.end(), {at(i), at(i + 1), at(i + 2)});
  } else if (d.prim == D3DPT_TRIANGLEFAN) {
    for (DWORD i = 1; i + 1 < n; ++i) tile.idx.insert(tile.idx.end(), {at(0), at(i), at(i + 1)});
  } else if (d.prim == D3DPT_TRIANGLESTRIP) {
    for (DWORD i = 0; i + 2 < n; ++i) tile.idx.insert(tile.idx.end(), {at(i), at(i + 1), at(i + 2)});
  }
  if (Cfg().log && g_sky_logged < 6) {
    ++g_sky_logged;
    Log("reflections: sky tile '%s' kept (%zu tiles): %lu verts, %zu tris, prim %lu, world translate (%.1f %.1f %.1f) "
        "cam (%.1f %.1f %.1f), extent (%.1f..%.1f, %.1f..%.1f, %.1f..%.1f), zenable %lu zwrite %lu fog %lu",
        name.c_str(), g_sky.size() + 1, static_cast<unsigned long>(d.count), tile.idx.size() / 3,
        static_cast<unsigned long>(d.prim), world_.m[3][0], world_.m[3][1], world_.m[3][2], cam[0], cam[1], cam[2],
        lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], rs_[RS_ZENABLE], rs_[RS_ZWRITEENABLE], rs_[RS_FOGENABLE]);
  }
  g_sky.push_back(std::move(tile));
  g_sky_dirty = true;
}

// The draw was just drawn: draw it again with the reflection on top.
void Device::ReflectPass(const Draw& d) {
  const double t0 = Now();
  if (!MakeGpu()) return;
  g_saw_3d = true;
  const TexInfo& ti = TexFor(tex_[0]);
  const size_t stride = d.layout.stride;
  const uint8_t* base = d.verts + static_cast<size_t>(d.first) * stride;
  const uint8_t* gv = (d.game_verts ? d.game_verts : d.verts) + static_cast<size_t>(d.first) * stride;
  const int uvo = d.layout.ntex > 0 ? d.layout.tex[0] : -1;

  // Vertices with their strength: the frame's if it is listed, else the
  // texture's.
  g_pv.resize(d.count);
  auto it = g_ranges.end();
  float sum[3] = {}, cnt = 0;
  for (DWORD i = 0; i < d.count; ++i) {
    const float* p = reinterpret_cast<const float*>(base + i * stride);
    PassVertex& v = g_pv[i];
    v.pos[0] = p[0], v.pos[1] = p[1], v.pos[2] = p[2];
    v.n[0] = v.n[1] = v.n[2] = 0;
    if (uvo >= 0) {
      const float* uv = reinterpret_cast<const float*>(base + i * stride + uvo);
      v.uv[0] = uv[0], v.uv[1] = uv[1];
    } else {
      v.uv[0] = v.uv[1] = 0;
    }
    float k = g_liquid_now ? 1.0f : ti.k;
    int mat = ti.mat;
    if (!g_ranges.empty()) {
      const uintptr_t a = reinterpret_cast<uintptr_t>(gv + i * stride);
      if (it == g_ranges.end() || a < it->first || a >= it->second.end) {
        it = g_ranges.upper_bound(a);
        if (it != g_ranges.begin()) --it;
        if (it != g_ranges.end() && (a < it->first || a >= it->second.end)) it = g_ranges.end();
      }
      if (it != g_ranges.end() && it->second.k >= 0 && !g_liquid_now) {
        k = it->second.k;
        mat = it->second.mat;
      }
    }
    v.k = k;
    v.f0 = Cfg().mats[mat].f0;
    v.blur = Cfg().mats[mat].blur;
    v.sun = k > 0 ? Cfg().mats[mat].sun : 0.0f;
    v.shine = Cfg().mats[mat].shine;
    if (k > 0) {
      sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
      cnt += 1;
    }
  }

  // Triangles with any reflective corner.
  g_pi.clear();
  const DWORD n = d.idx ? d.nidx : d.count;
  auto at = [&](DWORD i) -> uint16_t { return static_cast<uint16_t>(d.idx ? d.idx[i] : i); };
  auto add = [&](uint16_t a, uint16_t b, uint16_t c) {
    if (a >= d.count || b >= d.count || c >= d.count) return;
    if (g_pv[a].k <= 0 && g_pv[b].k <= 0 && g_pv[c].k <= 0) return;
    g_pi.push_back(a), g_pi.push_back(b), g_pi.push_back(c);
  };
  if (d.prim == D3DPT_TRIANGLELIST) {
    for (DWORD i = 0; i + 2 < n; i += 3) add(at(i), at(i + 1), at(i + 2));
  } else if (d.prim == D3DPT_TRIANGLEFAN) {
    for (DWORD i = 1; i + 1 < n; ++i) add(at(0), at(i), at(i + 1));
  } else if (d.prim == D3DPT_TRIANGLESTRIP) {
    for (DWORD i = 0; i + 2 < n; ++i) {
      if (i & 1) add(at(i + 1), at(i), at(i + 2));
      else add(at(i), at(i + 1), at(i + 2));
    }
  }
  if (g_pi.empty()) {
    ++g_st.skipped_no_tris;
    g_st.pass_s += Now() - t0;
    return;
  }

  // Normals: welded vertex groups, area-weighted face normals.
  const uint64_t key = WeldKey(d.game_verts ? d.game_verts : d.verts, d.count, d.idx, d.nidx, d.prim);
  Weld& w = g_welds[key];
  if (w.group.size() != d.count) {
    w.group.assign(d.count, 0);
    std::unordered_map<uint64_t, int> at_pos;
    at_pos.reserve(d.count * 2);
    w.groups = 0;
    for (DWORD i = 0; i < d.count; ++i) {
      uint32_t b[3];
      std::memcpy(b, g_pv[i].pos, 12);
      const uint64_t pk = (static_cast<uint64_t>(b[0]) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(b[1]) << 21) ^
                          (static_cast<uint64_t>(b[2]) * 0xC2B2AE3D27D4EB4Full);
      auto f = at_pos.find(pk);
      if (f == at_pos.end()) {
        at_pos.emplace(pk, w.groups);
        w.group[i] = w.groups++;
      } else {
        w.group[i] = f->second;
      }
    }
    if (g_welds.size() > 4096) {
      // Too many meshes remembered (memory reused): start over, keep this one.
      Weld keep = w;
      g_welds.clear();
      g_welds[key] = keep;
    }
  }
  Weld& wk = g_welds[key];
  g_acc.assign(static_cast<size_t>(wk.groups) * 3, 0.0f);
  for (size_t i = 0; i + 2 < g_pi.size(); i += 3) {
    const float* a = g_pv[g_pi[i]].pos;
    const float* b = g_pv[g_pi[i + 1]].pos;
    const float* c = g_pv[g_pi[i + 2]].pos;
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2],
                nz = e1[0] * e2[1] - e1[1] * e2[0];
    for (int j = 0; j < 3; ++j) {
      float* s = &g_acc[static_cast<size_t>(wk.group[g_pi[i + j]]) * 3];
      s[0] += nx, s[1] += ny, s[2] += nz;
    }
  }
  for (DWORD i = 0; i < d.count; ++i) {
    const float* s = &g_acc[static_cast<size_t>(wk.group[i]) * 3];
    const float l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (l > 0) {
      g_pv[i].n[0] = s[0] / l, g_pv[i].n[1] = s[1] / l, g_pv[i].n[2] = s[2] / l;
    } else {
      g_pv[i].n[1] = 1;
    }
  }

  // Capture point for the cube: the middle of the object (bike or rider)
  // nearest the camera, from all its reflective vertices.
  float cam[3];
  CameraPos(view_, cam);
  if (cnt > 0) {
    float c[3] = {sum[0] / cnt, sum[1] / cnt, sum[2] / cnt}, wc[3];
    Xform(c, world_, wc);
    ObjAcc& oa = g_obj_acc[g_cur_obj];
    for (int j = 0; j < 3; ++j) oa.sum[j] += wc[j] * cnt;
    oa.n += cnt;
  }

  if (!g.cube_valid) {
    g_st.pass_s += Now() - t0;
    return;  // nothing to reflect yet
  }

  // Upload and draw.
  Backend& b = B();
  const UINT vbytes = d.count * sizeof(PassVertex);
  const UINT ibytes = static_cast<UINT>(g_pi.size() * 2);
  const UINT voff = Push(g.vb, g.vb_size, g.vb_pos, vbytes, sizeof(PassVertex), [&](uint8_t* dst) { std::memcpy(dst, g_pv.data(), vbytes); });
  const UINT ioff = Push(g.ib, g.ib_size, g.ib_pos, ibytes, 4, [&](uint8_t* dst) { std::memcpy(dst, g_pi.data(), ibytes); });
  if (voff == UINT(-1) || ioff == UINT(-1)) return;
  PassConstants pc{};
  std::memcpy(pc.wvp, Wvp(), sizeof(pc.wvp));
  std::memcpy(pc.world, &world_, sizeof(pc.world));
  pc.vp[0] = static_cast<float>(viewport_.dwX);
  pc.vp[1] = static_cast<float>(viewport_.dwY);
  pc.vp[2] = static_cast<float>(viewport_.dwWidth ? viewport_.dwWidth : 1);
  pc.vp[3] = static_cast<float>(viewport_.dwHeight ? viewport_.dwHeight : 1);
  pc.cam[0] = cam[0], pc.cam[1] = cam[1], pc.cam[2] = cam[2];
  if (rs_[RS_FOGENABLE]) {
    const DWORD mode = rs_[RS_FOGTABLEMODE] ? rs_[RS_FOGTABLEMODE] : rs_[RS_FOGVERTEXMODE];
    const float scale = GetConfig().fog_scale;
    float v;
    std::memcpy(&v, &rs_[RS_FOGSTART], 4);
    pc.fog[0] = v * scale;
    std::memcpy(&v, &rs_[RS_FOGEND], 4);
    pc.fog[1] = v * scale;
    std::memcpy(&v, &rs_[RS_FOGDENSITY], 4);
    pc.fog[2] = v / scale;
    pc.fog[3] = static_cast<float>(mode & 3);
  }
  // ReflectDebug: the reflection alone at full strength (no Fresnel), to
  // see what is reflected.
  const bool full = Cfg().debug || g_liquid_now;
  pc.params[0] = full ? 1.0f : Cfg().strength;
  pc.params[1] = 0;
  pc.params[2] = full ? 1.0f : 0.0f;
  pc.params[3] = tex_[0] && (rs_[RS_ALPHATESTENABLE] || rs_[RS_COLORKEYENABLE] || rs_[RS_ALPHABLENDENABLE]) ? 0.5f : 0.0f;
  // The sun highlight: the race's shadow light (gpushadow.cpp).
  if (!full && Cfg().sun > 0 && ShadowSunDirection(pc.sun)) {
    pc.sun[3] = Cfg().sun;
    ++g_st.sun_draws;
    if (Cfg().log && std::memcmp(g_logged_sun, pc.sun, sizeof(g_logged_sun)) != 0) {
      std::memcpy(g_logged_sun, pc.sun, sizeof(g_logged_sun));
      Log("reflections: sun highlight from the race's shadow light: towards the sun (%.3f %.3f %.3f), %.1f deg up",
          pc.sun[0], pc.sun[1], pc.sun[2], std::asin(std::clamp(pc.sun[1], -1.0f, 1.0f)) * 57.29578f);
    }
  }
  b.ctx->UpdateSubresource(g.cb, 0, nullptr, &pc, 0, 0);
  const UINT vstride = sizeof(PassVertex), zero = 0;
  b.ctx->IASetInputLayout(g.layout);
  b.ctx->IASetVertexBuffers(0, 1, &g.vb, &vstride, &zero);
  b.ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R16_UINT, 0);
  b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  b.ctx->VSSetShader(g.vs, nullptr, 0);
  b.ctx->PSSetShader(g.ps, nullptr, 0);
  b.ctx->VSSetConstantBuffers(2, 1, &g.cb);
  b.ctx->PSSetConstantBuffers(2, 1, &g.cb);
  const GpuTexture* bt = tex_[0] ? tex_[0]->SampleTex() : nullptr;
  ID3D11ShaderResourceView* base_srv = bt ? bt->srv : nullptr;
  b.ctx->PSSetShaderResources(0, 1, &base_srv);
  b.ctx->PSSetShaderResources(3, 1, &g.cube_srv);
  b.ctx->PSSetSamplers(3, 1, &g.env_smp);
  const DWORD cull = rs_[RS_CULLMODE];
  b.ctx->RSSetState(g.raster[cull == D3DCULL_CW ? 1 : cull == D3DCULL_CCW ? 2 : 0]);
  b.ctx->OMSetBlendState(g.blend, nullptr, 0xffffffff);
  b.ctx->OMSetDepthStencilState(g.depth_eq, 0);
  b.ctx->DrawIndexed(static_cast<UINT>(g_pi.size()), ioff / 2, static_cast<INT>(voff / sizeof(PassVertex)));
  ID3D11ShaderResourceView* none = nullptr;
  b.ctx->PSSetShaderResources(3, 1, &none);
  ContextTouched();
  ++g_st.passes;
  g_st.tris += static_cast<unsigned>(g_pi.size() / 3);
  g_st.pass_s += Now() - t0;
}

void Device::ReflectRecord(const ReflectReplay& r) {
  const double t0 = Now();
  g_replays.push_back(r);
  for (auto* s : r.srv)
    if (s) s->AddRef();
  ++g_st.records;
  g_st.record_s += Now() - t0;
}

void ReflectForget() {
  ClearCache();
}

bool Device::ReflectRecording() const { return Cfg().mode == 2 && !IsD3D9(); }

void Device::ReflectCache(const ReflectReplay& r, const uint8_t* cpu, UINT cpu_stride, const ReflectSub* subs,
                          size_t nsubs, UINT gpu_stride) {
  if (!Cfg().cache || Cfg().mode != 2 || !cpu || !nsubs || !vbuf_ || (r.indexed && !ibuf_)) return;
  if (!MakeGpu() || g.cache_failed) return;
  const double t0 = Now();
  Backend& b = B();
  if (!g.cvb) {
    const UINT total = static_cast<UINT>(Cfg().cache_mb) << 20;
    g.cvb_size = total / 4 * 3 / gpu_stride * gpu_stride;
    g.cib_size = total / 4 / 4 * 4;
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = g.cvb_size;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    HRESULT hr = b.device->CreateBuffer(&bd, nullptr, &g.cvb);
    bd.ByteWidth = g.cib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (SUCCEEDED(hr)) hr = b.device->CreateBuffer(&bd, nullptr, &g.cib);
    if (FAILED(hr)) {
      Log("reflections: draw cache buffers (%d MB) failed %08lx; no cache", Cfg().cache_mb, hr);
      Rel(g.cvb), Rel(g.cib);
      g.cache_failed = true;
      return;
    }
    g.cvb_pos = g.cib_pos = 0;
    if (Cfg().log) Log("reflections: draw cache %u KB vertices, %u KB indices", g.cvb_size >> 10, g.cib_size >> 10);
  }
  const double now = Now();
  float w[16];
  std::memcpy(w, r.world, sizeof(w));
  for (size_t si = 0; si < nsubs; ++si) {
    const ReflectSub& sd = subs[si];
    if (!sd.nverts || (r.indexed && !sd.nidx)) continue;
    const uint8_t* pv = cpu + static_cast<size_t>(sd.vstart) * cpu_stride;
    // Key: texture, world, sizes, and where it is (the bounds of a few of
    // its vertices, rounded): terrain that morphs between detail levels as
    // the camera moves stays the same entry.
    float slo[3] = {1e30f, 1e30f, 1e30f}, shi[3] = {-1e30f, -1e30f, -1e30f};
    const DWORD step = sd.nverts > 16 ? sd.nverts / 16 : 1;
    auto take = [&](DWORD i) {
      float p[3];
      std::memcpy(p, pv + static_cast<size_t>(i) * cpu_stride, 12);
      for (int k = 0; k < 3; ++k) slo[k] = std::min(slo[k], p[k]), shi[k] = std::max(shi[k], p[k]);
    };
    for (DWORD i = 0; i < sd.nverts; i += step) take(i);
    take(sd.nverts - 1);
    int32_t q[6];
    for (int k = 0; k < 3; ++k) {
      q[k] = static_cast<int32_t>(std::floor(slo[k] * (1.0f / kCacheGrid)));
      q[3 + k] = static_cast<int32_t>(std::floor(shi[k] * (1.0f / kCacheGrid)));
    }
    uint64_t h = 1469598103934665603ull;
    h = Fnv(h, &r.srv[0], sizeof(r.srv[0]));
    h = Fnv(h, w, sizeof(w));
    h = Fnv(h, &sd.nverts, sizeof(sd.nverts));
    h = Fnv(h, &sd.nidx, sizeof(sd.nidx));
    h = Fnv(h, &r.topo, sizeof(r.topo));
    h = Fnv(h, q, sizeof(q));
    auto it = g_cache.find(h);
    if (it != g_cache.end()) {
      CacheEntry& e = it->second;
      e.seen = now;
      e.seen_frame = g_cache_frame;
      // Latest state (the textures are part of the key, the second may change).
      ID3D11ShaderResourceView* old[2] = {e.st.srv[0], e.st.srv[1]};
      e.st = r;
      for (auto* sv : e.st.srv)
        if (sv) sv->AddRef();
      for (auto* sv : old)
        if (sv) sv->Release();
      ++g_cs.hits;
      continue;
    }
    const UINT vbytes = sd.nverts * gpu_stride, ibytes = r.indexed ? sd.nidx * 2 : 0;
    if (vbytes > g.cvb_size / 8 || ibytes > g.cib_size / 8) {
      ++g_cs.too_big;
      continue;
    }
    CacheEntry e;
    e.st = r;
    e.count = sd.nverts;
    e.nidx = r.indexed ? sd.nidx : 0;
    e.vstart = static_cast<INT>(sd.vstart);
    // World-space bounds.
    for (int k = 0; k < 3; ++k) e.lo[k] = 1e30f, e.hi[k] = -1e30f;
    for (DWORD i = 0; i < sd.nverts; ++i) {
      float p[3];
      std::memcpy(p, pv + static_cast<size_t>(i) * cpu_stride, 12);
      for (int k = 0; k < 3; ++k) {
        const float q = p[0] * w[k] + p[1] * w[4 + k] + p[2] * w[8 + k] + w[12 + k];
        e.lo[k] = std::min(e.lo[k], q);
        e.hi[k] = std::max(e.hi[k], q);
      }
    }
    if (!(e.lo[0] <= e.hi[0])) continue;
    CacheAlloc(vbytes, ibytes, gpu_stride, &e.vbyte, &e.ibyte);
    e.vbytes = vbytes;
    e.ibytes = ibytes;
    const UINT src_v = static_cast<UINT>(r.base_vertex + static_cast<INT>(sd.vstart)) * gpu_stride;
    D3D11_BOX box{src_v, 0, 0, src_v + vbytes, 1, 1};
    b.ctx->CopySubresourceRegion(g.cvb, 0, e.vbyte, 0, 0, vbuf_, 0, &box);
    if (ibytes) {
      const UINT src_i = r.ioff + sd.istart * 2;
      D3D11_BOX ib{src_i, 0, 0, src_i + ibytes, 1, 1};
      b.ctx->CopySubresourceRegion(g.cib, 0, e.ibyte, 0, 0, ibuf_, 0, &ib);
    }
    for (auto* sv : e.st.srv)
      if (sv) sv->AddRef();
    e.seen = now;
    e.seen_frame = g_cache_frame;
    e.serial = ++g_cache_serial;
    g_cache.emplace(h, e);
    g_cache_order.emplace_back(h, e.serial);
    ++g_cs.inserts;
    g_cs.copied += vbytes + ibytes;
  }
  g_st.record_s += Now() - t0;
}

void Device::ReflectEndScene(ID3D11InputLayout* game_layout, UINT game_stride) {
  if (!Cfg().mode || IsD3D9()) return;
  const double t0 = Now();
  ++g_frame;
  const bool any = g_saw_3d || !g_replays.empty() || g_sky_dirty;
  g_saw_3d = false;
  if (any && MakeGpu()) {
    Backend& b = B();
    // Which faces now.
    bool faces[6] = {};
    int nfaces = 0;
    if (!g.cube_valid || (g_sky_dirty && Cfg().mode == 1 && g_frame % 8 == 0) || (g_sky_dirty && !g.cube_valid)) {
      for (bool& f : faces) f = true;
      nfaces = 6;
    } else if (Cfg().mode == 2) {
      // A face is only redrawn when every world draw of the frame can still
      // be replayed (the device's buffers were not started over meanwhile);
      // else it keeps its last picture and is redrawn next frame.
      bool stale = false;
      for (const ReflectReplay& r : g_replays)
        if (r.vgen != vgen_ || (r.indexed && r.igen != igen_)) {
          stale = true;
          break;
        }
      if (stale) {
        ++g_st.faces_skipped;
        goto no_faces;
      }
      for (int i = 0; i < Cfg().live_faces; ++i) {
        faces[g_face_cursor] = true;
        g_face_cursor = (g_face_cursor + 1) % 6;
      }
      nfaces = Cfg().live_faces;
    }
  no_faces:
    if (g_sky.empty() && g_replays.empty()) nfaces = 0;
    if (nfaces) {
      if (Cfg().mode == 1 || nfaces == 6) g_sky_dirty = false;
      float eye[3], cam[3], best = 1e30f;
      CameraPos(view_, cam);
      for (const auto& kv : g_obj_acc) {
        if (kv.second.n <= 0) continue;
        float c[3] = {kv.second.sum[0] / kv.second.n, kv.second.sum[1] / kv.second.n, kv.second.sum[2] / kv.second.n};
        const float dd = (c[0] - cam[0]) * (c[0] - cam[0]) + (c[1] - cam[1]) * (c[1] - cam[1]) + (c[2] - cam[2]) * (c[2] - cam[2]);
        if (dd < best) {
          best = dd;
          std::memcpy(eye, c, sizeof(eye));
        }
      }
      if (best < 1e29f) {
        std::memcpy(g_last_eye, eye, sizeof(eye));
        g_have_eye = true;
        ++g_st.capture_bike;
      } else if (g_have_eye) {
        std::memcpy(eye, g_last_eye, sizeof(eye));
        ++g_st.capture_bike;
      } else {
        std::memcpy(eye, cam, sizeof(eye));
        ++g_st.capture_cam;
      }
      // Near/far from the game's projection.
      float n = 1.0f, f = 10000.0f;
      if (proj_.m[2][3] != 0 && proj_.m[2][2] != 0 && proj_.m[2][2] != 1) {
        n = -proj_.m[3][2] / proj_.m[2][2];
        f = -proj_.m[3][2] / (proj_.m[2][2] - 1);
        if (!(n > 0.01f && n < 100.0f)) n = 1.0f;
        if (!(f > n * 2 && f < 1e6f)) f = 10000.0f;
      }
      // Cached draws (ReflectCache) for this update: not drawn this frame,
      // recent, near the bike, and out of the camera's view (what is in view
      // was drawn this frame if the game still draws it).
      g_cache_draw.clear();
      if (Cfg().cache && !g_cache.empty() && g.cvb) {
        float radius = Cfg().cache_radius;
        if (radius <= 0) {
          float fe = 0;
          std::memcpy(&fe, &rs_[RS_FOGEND], 4);
          fe *= GetConfig().fog_scale;
          radius = rs_[RS_FOGENABLE] && fe > 1 ? fe : f;
        }
        g_cache_radius_used = radius;
        float cvp[16];
        Mul(&view_.m[0][0], &proj_.m[0][0], cvp);
        const double now = Now();
        for (auto& kv : g_cache) {
          const CacheEntry& e = kv.second;
          if (e.seen_frame == g_cache_frame) continue;
          if (now - e.seen > Cfg().cache_secs) {
            ++g_cs.culled_old;
            continue;
          }
          if (BoxDist2(e.lo, e.hi, eye) > radius * radius) {
            ++g_cs.culled_far;
            continue;
          }
          if (!BoxOutside(e.lo, e.hi, cvp)) {
            ++g_cs.culled_view;
            continue;
          }
          g_cache_draw.push_back(&e);
        }
      }
      float proj[16], sky_proj[16];
      FaceProj(n, f, proj);
      FaceProj(0.5f, 1e6f, sky_proj);
      float fogc[4];
      const DWORD fc = rs_[RS_FOGCOLOR];
      fogc[0] = ((fc >> 16) & 0xff) / 255.0f * Cfg().ground;
      fogc[1] = ((fc >> 8) & 0xff) / 255.0f * Cfg().ground;
      fogc[2] = (fc & 0xff) / 255.0f * Cfg().ground;
      fogc[3] = 1;
      D3D11_VIEWPORT vp{0, 0, static_cast<float>(g.size), static_cast<float>(g.size), 0, 1};
      b.ctx->RSSetViewports(1, &vp);
      const unsigned vgen = vgen_, igen = igen_;
      const float origin[3] = {0, 0, 0};
      UINT sky_vo = UINT(-1), sky_io = UINT(-1);
      for (int face = 0; face < 6; ++face) {
        if (!faces[face]) continue;
        ++g_st.faces;
        b.ctx->OMSetRenderTargets(1, &g.face_rtv[face], g.dsv);
        b.ctx->ClearRenderTargetView(g.face_rtv[face], fogc);
        b.ctx->ClearDepthStencilView(g.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        // Sky.
        if (!g_sky.empty()) {
          float view[16], m[16];
          FaceView(face, origin, view);
          Mul(view, sky_proj, m);
          b.ctx->UpdateSubresource(g.sky_cb, 0, nullptr, m, 0, 0);
          const UINT stride = sizeof(SkyVertex), zero = 0;
          b.ctx->IASetInputLayout(g.sky_layout);
          b.ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &zero);
          b.ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R16_UINT, 0);
          b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          b.ctx->VSSetShader(g.sky_vs, nullptr, 0);
          b.ctx->PSSetShader(g.sky_ps, nullptr, 0);
          b.ctx->VSSetConstantBuffers(3, 1, &g.sky_cb);
          b.ctx->PSSetSamplers(5, 1, &g.clamp_smp);
          b.ctx->RSSetState(g.raster_none);
          b.ctx->OMSetBlendState(g.opaque, nullptr, 0xffffffff);
          b.ctx->OMSetDepthStencilState(g.depth_off, 0);
          if (sky_vo == UINT(-1)) {
            // All tiles in one upload per cube update.
            UINT vb = 0, ib = 0;
            for (const SkyTile& t : g_sky) {
              vb += static_cast<UINT>(t.v.size() * sizeof(SkyVertex));
              ib += static_cast<UINT>(t.idx.size() * 2);
            }
            sky_vo = Push(g.vb, g.vb_size, g.vb_pos, vb, sizeof(SkyVertex), [&](uint8_t* dst) {
              for (const SkyTile& t : g_sky) {
                std::memcpy(dst, t.v.data(), t.v.size() * sizeof(SkyVertex));
                dst += t.v.size() * sizeof(SkyVertex);
              }
            });
            sky_io = Push(g.ib, g.ib_size, g.ib_pos, ib, 4, [&](uint8_t* dst) {
              for (const SkyTile& t : g_sky) {
                std::memcpy(dst, t.idx.data(), t.idx.size() * 2);
                dst += t.idx.size() * 2;
              }
            });
          }
          if (sky_vo != UINT(-1) && sky_io != UINT(-1)) {
            UINT vat = sky_vo / sizeof(SkyVertex), iat = sky_io / 2;
            for (const SkyTile& t : g_sky) {
              if (!t.idx.empty()) {
                b.ctx->PSSetShaderResources(5, 1, &t.srv);
                b.ctx->DrawIndexed(static_cast<UINT>(t.idx.size()), iat, static_cast<INT>(vat));
                ++g_st.sky_tiles_drawn;
              }
              vat += static_cast<UINT>(t.v.size());
              iat += static_cast<UINT>(t.idx.size());
            }
          }
        }
        // The frame's world draws, from the bike.
        if (Cfg().mode == 2 && !g_replays.empty() && game_layout) {
          float view[16], vpm[16];
          FaceView(face, eye, view);
          Mul(view, proj, vpm);
          b.ctx->IASetInputLayout(game_layout);
          const UINT zero = 0;
          b.ctx->IASetVertexBuffers(0, 1, &vbuf_, &game_stride, &zero);
          b.ctx->IASetIndexBuffer(ibuf_, DXGI_FORMAT_R16_UINT, 0);
          b.ctx->VSSetConstantBuffers(0, 1, &g.game_vs_cb);
          b.ctx->PSSetConstantBuffers(1, 1, &ps_cb_);
          b.ctx->RSSetState(g.raster_none);
          GameVSConstants vc{};
          vc.vp[2] = vc.vp[3] = static_cast<float>(g.size);
          float last_world[16];
          bool have_world = false;
          const ReflectReplay* prev = nullptr;
          for (const ReflectReplay& r : g_replays) {
            if (r.vgen != vgen || (r.indexed && r.igen != igen)) {
              ++g_st.replay_stale;
              continue;
            }
            if (!have_world || std::memcmp(last_world, r.world, sizeof(last_world)) != 0) {
              Mul(r.world, vpm, vc.wvp);
              b.ctx->UpdateSubresource(g.game_vs_cb, 0, nullptr, &vc, 0, 0);
              std::memcpy(last_world, r.world, sizeof(last_world));
              have_world = true;
            }
            if (!prev || prev->blend != r.blend) b.ctx->OMSetBlendState(r.blend, nullptr, 0xffffffff);
            if (!prev || prev->depth != r.depth) b.ctx->OMSetDepthStencilState(r.depth, 0);
            if (!prev || std::memcmp(prev->srv, r.srv, sizeof(r.srv)) != 0) b.ctx->PSSetShaderResources(0, 2, r.srv);
            if (!prev || std::memcmp(prev->smp, r.smp, sizeof(r.smp)) != 0) b.ctx->PSSetSamplers(0, 3, r.smp);
            if (!prev || prev->vs != r.vs) b.ctx->VSSetShader(r.vs, nullptr, 0);
            if (!prev || prev->ps != r.ps) b.ctx->PSSetShader(r.ps, nullptr, 0);
            if (!prev || prev->pc_size != r.pc_size || std::memcmp(prev->pc, r.pc, r.pc_size) != 0)
              b.ctx->UpdateSubresource(ps_cb_, 0, nullptr, r.pc, 0, 0);
            if (!prev || prev->topo != r.topo) b.ctx->IASetPrimitiveTopology(r.topo);
            if (r.indexed) b.ctx->DrawIndexed(r.nidx, r.ioff / 2, r.base_vertex);
            else b.ctx->Draw(r.count, static_cast<UINT>(r.base_vertex));
            prev = &r;
            ++g_st.replays;
          }
        }
        // Cached draws from earlier frames (behind the camera).
        if (Cfg().mode == 2 && !g_cache_draw.empty() && game_layout) {
          float view[16], vpm[16];
          FaceView(face, eye, view);
          Mul(view, proj, vpm);
          b.ctx->IASetInputLayout(game_layout);
          const UINT zero = 0;
          b.ctx->IASetVertexBuffers(0, 1, &g.cvb, &game_stride, &zero);
          b.ctx->IASetIndexBuffer(g.cib, DXGI_FORMAT_R16_UINT, 0);
          b.ctx->VSSetConstantBuffers(0, 1, &g.game_vs_cb);
          b.ctx->PSSetConstantBuffers(1, 1, &ps_cb_);
          b.ctx->RSSetState(g.raster_none);
          GameVSConstants vc{};
          vc.vp[2] = vc.vp[3] = static_cast<float>(g.size);
          float last_world[16];
          bool have_world = false;
          const ReflectReplay* prev = nullptr;
          for (const CacheEntry* ce : g_cache_draw) {
            const ReflectReplay& r = ce->st;
            if (!have_world || std::memcmp(last_world, r.world, sizeof(last_world)) != 0) {
              Mul(r.world, vpm, vc.wvp);
              b.ctx->UpdateSubresource(g.game_vs_cb, 0, nullptr, &vc, 0, 0);
              std::memcpy(last_world, r.world, sizeof(last_world));
              have_world = true;
            }
            if (!prev || prev->blend != r.blend) b.ctx->OMSetBlendState(r.blend, nullptr, 0xffffffff);
            if (!prev || prev->depth != r.depth) b.ctx->OMSetDepthStencilState(r.depth, 0);
            if (!prev || std::memcmp(prev->srv, r.srv, sizeof(r.srv)) != 0) b.ctx->PSSetShaderResources(0, 2, r.srv);
            if (!prev || std::memcmp(prev->smp, r.smp, sizeof(r.smp)) != 0) b.ctx->PSSetSamplers(0, 3, r.smp);
            if (!prev || prev->vs != r.vs) b.ctx->VSSetShader(r.vs, nullptr, 0);
            if (!prev || prev->ps != r.ps) b.ctx->PSSetShader(r.ps, nullptr, 0);
            if (!prev || prev->pc_size != r.pc_size || std::memcmp(prev->pc, r.pc, r.pc_size) != 0)
              b.ctx->UpdateSubresource(ps_cb_, 0, nullptr, r.pc, 0, 0);
            if (!prev || prev->topo != r.topo) b.ctx->IASetPrimitiveTopology(r.topo);
            const INT vat = static_cast<INT>(ce->vbyte / game_stride);
            if (ce->ibytes) b.ctx->DrawIndexed(ce->nidx, ce->ibyte / 2, vat - ce->vstart);
            else b.ctx->Draw(ce->count, static_cast<UINT>(vat));
            prev = &r;
            ++g_cs.drawn;
          }
        }
      }
      ID3D11RenderTargetView* no_rt = nullptr;
      b.ctx->OMSetRenderTargets(1, &no_rt, nullptr);
      b.ctx->GenerateMips(g.cube_srv);
      g.cube_valid = true;
      ++g_st.cube_builds;
      ContextTouched();
    }
  }
  for (ReflectReplay& r : g_replays)
    for (auto* s : r.srv)
      if (s) s->Release();
  g_replays.clear();
  g_obj_acc.clear();
  ++g_cache_frame;
  // Old entries go (their textures with them), a little at a time.
  if (g_cache_frame % 30 == 0 && !g_cache.empty()) {
    const double now = Now();
    for (auto it = g_cache.begin(); it != g_cache.end();) {
      if (now - it->second.seen > Cfg().cache_secs) {
        ReleaseEntry(it->second);
        it = g_cache.erase(it);
        ++g_cs.aged;
      } else {
        ++it;
      }
    }
  }
  g_st.cube_s += Now() - t0;

  if (any) ++g_st.frames;
  const double now = Now();
  if (!g_st_since) g_st_since = now;
  if (now - g_st_since >= 5.0) {
    if (Cfg().log && g_st.frames) {
      const double fr = g_st.frames;
      Log("reflections 5s: %u frames; pass %.3f ms/frame (%.1f draws, %.0f tris, %u skipped); cube %.3f ms/frame "
          "(%.2f faces, %u face updates put off, %.1f replayed draws, %u stale, %u sky tiles kept, %u builds), record %.3f ms/frame "
          "(%.1f draws); sun highlight on %u of %u draws; cube from bike %u / camera %u; hooks: %u object draws, %u part transforms, %zu ranges",
          g_st.frames, g_st.pass_s * 1000 / fr, g_st.passes / fr, g_st.tris / fr, g_st.skipped_no_tris,
          g_st.cube_s * 1000 / fr, g_st.faces / fr, g_st.faces_skipped, g_st.replays / fr, g_st.replay_stale,
          static_cast<unsigned>(g_sky.size()), g_st.cube_builds, g_st.record_s * 1000 / fr, g_st.records / fr,
          g_st.sun_draws, g_st.passes, g_st.capture_bike, g_st.capture_cam, g_obj_draws, g_xforms, g_ranges.size());
      if (Cfg().mode == 2 && Cfg().cache) {
        size_t vb = 0, ib = 0;
        for (const auto& kv : g_cache) vb += kv.second.vbytes, ib += kv.second.ibytes;
        Log("reflections 5s cache: %zu draws kept (%.1f MB of %.1f), radius %.0f; per frame: %.1f found again, %.1f new "
            "(%.1f KB copied), %.1f cached draws drawn per cube update; left out: %u in view, %u too far, %u too old; "
            "%u pushed out (full), %u aged out, %u too big",
            g_cache.size(), (vb + ib) / 1048576.0, (g.cvb_size + g.cib_size) / 1048576.0, g_cache_radius_used,
            g_cs.hits / fr, g_cs.inserts / fr, g_cs.copied / 1024 / fr,
            g_st.cube_builds ? double(g_cs.drawn) / g_st.faces : 0.0, g_cs.culled_view, g_cs.culled_far,
            g_cs.culled_old, g_cs.evicted, g_cs.aged, g_cs.too_big);
      }
    }
    g_cs = CStats{};
    g_st = RStats{};
    g_obj_draws = g_xforms = 0;
    g_st_since = now;
  }
}

}  // namespace dd
