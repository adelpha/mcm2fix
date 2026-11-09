// Smoothing at the source: instead of blending what the game drew, give the
// game's own drawing code the in-between positions.
//
// Bikes, riders and their wheels are D3DIMSoultreeObjects. Each frame their
// render (0x4417b0, called once from 0x440fe1) rebuilds the world-space
// vertices from every part's world matrix (part+0xf8) with 0x4a3070 and,
// with lighting on, the normals with 0x49fa50 (both called in 0x441ce0). We
// hook those two calls and hand them each part's matrix blended between the
// last two simulation steps: rotation by quaternion (so wheels and crash
// tumbles stay rigid), position and scale linearly. Fast spins (wheels) can
// turn more than half a turn per step, so the way round is chosen to match
// the part's previous spin.
//
// Names over riders are NameOverlays (vtable 0x55d990). Their draw
// (0x51e760) projects the rider's head to the screen with 0x5349b0 (called at
// 0x51e980), using the step's camera and head; the result is blended between
// steps per overlay.
#include "smooth.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "framerate.h"
#include "patch.h"

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMaxPartMove = 60.0f;  // world units per step; more is a teleport

struct Quat { float w, x, y, z; };

Quat Mul(const Quat& a, const Quat& b) {
  return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
Quat Conj(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }

// Row-vector matrix (D3D): rows 0..2 are the basis. Treat A = R^T (column
// form): A[i][j] = r[j][i].
Quat FromRows(const float r[3][3]) {
  const float a00 = r[0][0], a01 = r[1][0], a02 = r[2][0];
  const float a10 = r[0][1], a11 = r[1][1], a12 = r[2][1];
  const float a20 = r[0][2], a21 = r[1][2], a22 = r[2][2];
  Quat q;
  const float tr = a00 + a11 + a22;
  if (tr > 0) {
    const float s = std::sqrt(tr + 1.0f) * 2;
    q = {0.25f * s, (a21 - a12) / s, (a02 - a20) / s, (a10 - a01) / s};
  } else if (a00 > a11 && a00 > a22) {
    const float s = std::sqrt(1.0f + a00 - a11 - a22) * 2;
    q = {(a21 - a12) / s, 0.25f * s, (a01 + a10) / s, (a02 + a20) / s};
  } else if (a11 > a22) {
    const float s = std::sqrt(1.0f + a11 - a00 - a22) * 2;
    q = {(a02 - a20) / s, (a01 + a10) / s, 0.25f * s, (a12 + a21) / s};
  } else {
    const float s = std::sqrt(1.0f + a22 - a00 - a11) * 2;
    q = {(a10 - a01) / s, (a02 + a20) / s, (a12 + a21) / s, 0.25f * s};
  }
  const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return {q.w / n, q.x / n, q.y / n, q.z / n};
}

void ToRows(const Quat& q, float r[3][3]) {
  const float w = q.w, x = q.x, y = q.y, z = q.z;
  const float a[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                         {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                         {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = a[j][i];
}

// Split a matrix into scale per row, rotation and translation. False if the
// basis is degenerate.
bool Decompose(const float* m, float scale[3], Quat* q) {
  float r[3][3];
  for (int i = 0; i < 3; ++i) {
    const float* row = m + i * 4;
    scale[i] = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
    if (scale[i] < 1e-6f) return false;
    for (int j = 0; j < 3; ++j) r[i][j] = row[j] / scale[i];
  }
  // Gram-Schmidt so rounding in the game's matrix can't skew the quaternion.
  float d = r[0][0] * r[1][0] + r[0][1] * r[1][1] + r[0][2] * r[1][2];
  for (int j = 0; j < 3; ++j) r[1][j] -= d * r[0][j];
  float n = std::sqrt(r[1][0] * r[1][0] + r[1][1] * r[1][1] + r[1][2] * r[1][2]);
  if (n < 1e-6f) return false;
  for (int j = 0; j < 3; ++j) r[1][j] /= n;
  float c[3] = {r[0][1] * r[1][2] - r[0][2] * r[1][1], r[0][2] * r[1][0] - r[0][0] * r[1][2],
                r[0][0] * r[1][1] - r[0][1] * r[1][0]};
  // Keep a mirrored basis mirrored.
  const float side = c[0] * r[2][0] + c[1] * r[2][1] + c[2] * r[2][2];
  if (side < 0) return false;
  for (int j = 0; j < 3; ++j) r[2][j] = c[j];
  *q = FromRows(r);
  return true;
}

// Rotation vector (axis * angle, angle in [0, pi]) of a unit quaternion.
void ToVector(Quat q, float v[3]) {
  if (q.w < 0) q = {-q.w, -q.x, -q.y, -q.z};
  const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (s < 1e-7f) { v[0] = v[1] = v[2] = 0; return; }
  const float angle = 2.0f * std::atan2(s, q.w);
  v[0] = q.x / s * angle; v[1] = q.y / s * angle; v[2] = q.z / s * angle;
}

Quat FromVector(const float v[3]) {
  const float angle = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (angle < 1e-7f) return {1, 0, 0, 0};
  const float s = std::sin(angle / 2) / angle;
  return {std::cos(angle / 2), v[0] * s, v[1] * s, v[2] * s};
}

struct PartRec {
  float prev[16] = {}, cur[16] = {};
  float spin[3] = {};  // rotation vector of the last step's turn
  bool spin_valid = false;
  unsigned prev_tick = 0, tick = 0;
  float out[16] = {};
  unsigned out_frame = ~0u;  // `out` is this frame's blend
  bool out_ok = false;
};
std::unordered_map<uintptr_t, PartRec> g_parts;
unsigned g_part_frame = 0;
bool g_drawing = false;  // inside the game's Draw for this frame
unsigned g_changed = 0;  // matrix changed within a frame
// Objects the game rebuilt (0x441ce0) during a race, with the tick it last did.
std::vector<uintptr_t> g_drawn;  // objects drawn in this frame's Draw (alive until it ends)
// Vertex ranges we filled with blended positions this frame.
std::vector<std::pair<uintptr_t, uintptr_t>> g_ranges;
// draw-start rebuilds, parts blended, unchanged, no previous, jumped, bad matrix, long way round,
// transform calls, normal calls, game rebuilds in sim, game rebuilds in draw, objects dropped (bad vtable),
// draws left to us
unsigned g_pw[13] = {};
bool g_part_sample = false;
float g_max_turn = 0.0f;

const float* BlendPart(const float* m) {
  unsigned tick;
  float alpha;
  if (!g_drawing || !SimDrawPoint(&tick, &alpha)) return m;
  PartRec& r = g_parts[reinterpret_cast<uintptr_t>(m)];
  if (r.tick != tick) {
    // New step: the turn just finished becomes the reference spin.
    if (r.tick != 0 && r.prev_tick + 1 == r.tick) {
      float s1[3];
      Quat q0, q1;
      if (Decompose(r.prev, s1, &q0) && Decompose(r.cur, s1, &q1)) {
        ToVector(Mul(q1, Conj(q0)), r.spin);
        r.spin_valid = true;
      } else {
        r.spin_valid = false;
      }
    } else {
      r.spin_valid = false;
    }
    std::memcpy(r.prev, r.cur, sizeof(r.prev));
    r.prev_tick = r.tick;
    r.tick = tick;
    r.out_frame = ~0u;
  }
  if (r.out_frame == g_part_frame) {
    if (std::memcmp(r.cur, m, sizeof(r.cur)) == 0) return r.out_ok ? r.out : m;
    ++g_changed;  // not the matrix we blended; leave it alone
    return m;
  }
  r.out_frame = g_part_frame;
  r.out_ok = false;
  std::memcpy(r.cur, m, sizeof(r.cur));
  if (r.prev_tick == 0 || r.prev_tick + 1 != tick) { ++g_pw[3]; return m; }
  if (std::memcmp(r.prev, r.cur, sizeof(r.cur)) == 0) { ++g_pw[2]; return m; }
  const float dx = r.cur[12] - r.prev[12], dy = r.cur[13] - r.prev[13], dz = r.cur[14] - r.prev[14];
  if (dx * dx + dy * dy + dz * dz > kMaxPartMove * kMaxPartMove) { ++g_pw[4]; return m; }
  float s0[3], s1[3];
  Quat q0, q1;
  if (!Decompose(r.prev, s0, &q0) || !Decompose(r.cur, s1, &q1)) { ++g_pw[5]; return m; }
  float turn[3];
  ToVector(Mul(q1, Conj(q0)), turn);
  const float angle = std::sqrt(turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2]);
  if (angle > 1e-6f && r.spin_valid) {
    // The same end pose is also reached by turning the other way round,
    // through 2*pi - angle. Pick the way closer to the last step's spin.
    const float k = (angle - 2 * kPi) / angle;
    const float other[3] = {turn[0] * k, turn[1] * k, turn[2] * k};
    float d1 = 0, d2 = 0;
    for (int i = 0; i < 3; ++i) {
      d1 += (turn[i] - r.spin[i]) * (turn[i] - r.spin[i]);
      d2 += (other[i] - r.spin[i]) * (other[i] - r.spin[i]);
    }
    if (d2 < d1) {
      std::memcpy(turn, other, sizeof(turn));
      ++g_pw[6];
    }
  }
  const float turned = std::sqrt(turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2]);
  if (turned > g_max_turn) g_max_turn = turned;
  const float part[3] = {turn[0] * alpha, turn[1] * alpha, turn[2] * alpha};
  const Quat q = Mul(FromVector(part), q0);
  float rot[3][3];
  ToRows(q, rot);
  for (int i = 0; i < 3; ++i) {
    const float s = s0[i] + (s1[i] - s0[i]) * alpha;
    for (int j = 0; j < 3; ++j) r.out[i * 4 + j] = rot[i][j] * s;
    r.out[i * 4 + 3] = r.prev[i * 4 + 3] + (r.cur[i * 4 + 3] - r.prev[i * 4 + 3]) * alpha;
  }
  for (int j = 0; j < 4; ++j) r.out[12 + j] = r.prev[12 + j] + (r.cur[12 + j] - r.prev[12 + j]) * alpha;
  r.out_ok = true;
  ++g_pw[1];
  if (!g_part_sample && turned > 0.5f) {
    g_part_sample = true;
    patch::Log("smoothing: part %08x turned %.1f deg this step (last step %.1f deg%s), at a=%.2f; "
               "pos (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) drawn (%.2f %.2f %.2f)",
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(m)), turned * 180 / kPi,
               r.spin_valid ? std::sqrt(r.spin[0] * r.spin[0] + r.spin[1] * r.spin[1] + r.spin[2] * r.spin[2]) * 180 / kPi
                            : 0.0f,
               r.spin_valid ? "" : ", unknown", alpha, r.prev[12], r.prev[13], r.prev[14], r.cur[12], r.cur[13],
               r.cur[14], r.out[12], r.out[13], r.out[14]);
  }
  return r.out;
}

// 0x4a3070(dst, src, matrix, count, dst stride, src stride), cdecl.
using XformFn = void(__cdecl*)(void*, const void*, const float*, int, int, int);
const XformFn kXform = reinterpret_cast<XformFn>(0x4A3070);
void __cdecl PartXform(void* dst, const void* src, const float* m, int n, int ds, int ss) {
  ++g_pw[7];
  const float* use = BlendPart(m);
  if (use != m && dst && n > 0) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(dst);
    g_ranges.emplace_back(a, a + static_cast<uintptr_t>(n) * static_cast<uintptr_t>(ds));
  }
  kXform(dst, src, use, n, ds, ss);
}

// 0x49fa50(matrix, count, a, b, c, d), thiscall on the lighting object.
using LightFn = int(__thiscall*)(void*, const float*, int, int, int, int, int);
const LightFn kLight = reinterpret_cast<LightFn>(0x49FA50);
int __fastcall PartLight(void* self, void*, const float* m, int n, int a, int b, int c, int d) {
  ++g_pw[8];
  return kLight(self, BlendPart(m), n, a, b, c, d);
}

// 0x441ce0(level): rebuild an object's world-space vertices. Hooked at its
// entry (trampoline) to learn which objects exist.
using RebuildFn = void(__thiscall*)(void*, int);
RebuildFn g_rebuild = nullptr;  // trampoline to the original

// Objects the game itself rebuilds during simulation steps, i.e. the ones
// that move (bikes, riders). Only these are rebuilt from blended parts at
// draw time: rebuilding a still object there throws away what the game did
// to it after its own rebuild (inactive gates lost their see-through look
// once the race started). Keyed by address only, never dereferenced.
struct SimRebuild {
  unsigned tick = 0;  // last step it was rebuilt in
  unsigned run = 0;   // consecutive steps
};
std::unordered_map<uintptr_t, SimRebuild> g_sim_rebuilt;
constexpr unsigned kMovingRun = 3;  // rebuilt in this many steps in a row = moving
unsigned g_still_skipped = 0, g_last_prune = 0;

void __fastcall RebuildHook(void* self, void*, int level) {
  unsigned tick;
  float alpha;
  if (SimDrawPoint(&tick, &alpha)) {
    ++g_pw[g_drawing ? 10 : 9];
    if (!g_drawing) {
      SimRebuild& r = g_sim_rebuilt[reinterpret_cast<uintptr_t>(self)];
      if (r.tick != tick) {
        r.run = (r.tick + 1 == tick) ? r.run + 1 : 1;
        r.tick = tick;
      }
      if (tick - g_last_prune > 600) {
        g_last_prune = tick;
        for (auto it = g_sim_rebuilt.begin(); it != g_sim_rebuilt.end();)
          it = (tick - it->second.tick > 300) ? g_sim_rebuilt.erase(it) : std::next(it);
      }
    }
  }
  g_rebuild(self, level);
}

bool IsMoving(uintptr_t obj, unsigned tick) {
  auto it = g_sim_rebuilt.find(obj);
  return it != g_sim_rebuilt.end() && it->second.run >= kMovingRun && it->second.tick + 2 >= tick;
}

// 0x441ed0: draw one object (thiscall, called from 0x444fea). It rebuilds
// the vertices first only if the object is marked dirty (+0x18c); while
// smoothing we mark it, so it is rebuilt from blended parts right here, for
// an object that certainly exists.
using DrawObjFn = void(__thiscall*)(void*);
const DrawObjFn kDrawObj = reinterpret_cast<DrawObjFn>(0x441ED0);
void __fastcall PartDraw(void* self, void*) {
  unsigned tick;
  float alpha;
  if (g_drawing && SimDrawPoint(&tick, &alpha) && !IsMoving(reinterpret_cast<uintptr_t>(self), tick)) {
    ++g_still_skipped;
  } else if (g_drawing) {
    ++g_pw[0];
    *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(self) + 0x18c) = 1;
    g_drawn.push_back(reinterpret_cast<uintptr_t>(self));
  }
  kDrawObj(self);
}

}  // namespace

void SmoothDrawBegin() {
  g_ranges.clear();
  g_drawn.clear();
  unsigned tick;
  float alpha;
  if (!g_rebuild || !SimDrawPoint(&tick, &alpha)) return;
  g_drawing = true;
}

void SmoothDrawEnd() {
  if (!g_drawing) return;
  g_drawing = false;
  // These hold the blend now: have the next reader rebuild from the step.
  for (uintptr_t o : g_drawn) *reinterpret_cast<int*>(o + 0x18c) = 1;
  g_drawn.clear();
}

bool SmoothPartsOwn(const void* verts) {
  const uintptr_t v = reinterpret_cast<uintptr_t>(verts);
  for (const auto& r : g_ranges)
    if (v >= r.first && v < r.second) { ++g_pw[12]; return true; }
  return false;
}

bool SmoothPartsActive() { return g_drawing; }

namespace {

// ---- names over riders ----

struct NameRec {
  float prev[2] = {}, cur[2] = {};
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uintptr_t, NameRec> g_names;
// projections, blended, no previous, jumped, failed
unsigned g_nw[6] = {};
bool g_name_sample = false;

using ProjectFn = int(__thiscall*)(void*, void*, void*, float*, float*, void*);
const ProjectFn kProject = reinterpret_cast<ProjectFn>(0x5349B0);

// ---- dropping text ("FINAL LAP") ----
// DropTextOverlay (vtable 0x55da74): its update (slot 10, 0x5206c0) moves the
// text once per step; its draw (slot 15, 0x520740) prints it at x +0x3c,
// y +0x44 (ints). Draw it at the position blended between the last two steps.
struct DropRec {
  int prev[2] = {}, cur[2] = {};
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uintptr_t, DropRec> g_drops;
unsigned g_dw[4] = {};  // draws, blended, no previous, jumped
bool g_drop_sample = false;
using DropDrawFn = int(__thiscall*)(void*);
const DropDrawFn kDropDraw = reinterpret_cast<DropDrawFn>(0x520740);

int __fastcall DropDraw(void* self, void*) {
  ++g_dw[0];
  unsigned tick;
  float alpha;
  if (!SimDrawPoint(&tick, &alpha)) return kDropDraw(self);
  const uintptr_t o = reinterpret_cast<uintptr_t>(self);
  int* pos[2] = {reinterpret_cast<int*>(o + 0x3c), reinterpret_cast<int*>(o + 0x44)};
  DropRec& r = g_drops[o];
  if (r.tick != tick) {
    r.prev[0] = r.cur[0];
    r.prev[1] = r.cur[1];
    r.prev_tick = r.tick;
    r.tick = tick;
  }
  r.cur[0] = *pos[0];
  r.cur[1] = *pos[1];
  if (r.prev_tick == 0 || r.prev_tick + 1 != tick) { ++g_dw[2]; return kDropDraw(self); }
  const int dx = r.cur[0] - r.prev[0], dy = r.cur[1] - r.prev[1];
  if (dx * dx + dy * dy > 300 * 300) { ++g_dw[3]; return kDropDraw(self); }
  const int bx = r.prev[0] + static_cast<int>(std::lround(dx * alpha));
  const int by = r.prev[1] + static_cast<int>(std::lround(dy * alpha));
  if (!g_drop_sample && (dx || dy)) {
    g_drop_sample = true;
    patch::Log("smoothing: drop text %08x at (%d %d) was (%d %d), drawn at a=%.2f (%d %d)", static_cast<unsigned>(o),
               r.cur[0], r.cur[1], r.prev[0], r.prev[1], alpha, bx, by);
  }
  *pos[0] = bx;
  *pos[1] = by;
  ++g_dw[1];
  const int result = kDropDraw(self);
  *pos[0] = r.cur[0];
  *pos[1] = r.cur[1];
  return result;
}

}  // namespace

extern "C" {
uintptr_t g_name_obj = 0;  // the NameOverlay being drawn (esi at the call)
void* g_name_project_fn = nullptr;
}

namespace {

int __fastcall NameProject(void* self, void*, void* cam, void* cam_ec, float* world, float* screen, void* extra) {
  const int ok = kProject(self, cam, cam_ec, world, screen, extra);
  ++g_nw[0];
  unsigned tick;
  float alpha;
  if (!ok) { ++g_nw[4]; return ok; }
  if (!SimDrawPoint(&tick, &alpha)) return ok;
  float ratio;
  if (SmoothCullMargin(reinterpret_cast<uintptr_t>(cam), &ratio)) {
    // The camera was widened for culling; put the name where the drawn view has it.
    const uintptr_t c = reinterpret_cast<uintptr_t>(cam);
    const float cx = *reinterpret_cast<int*>(c + 0x1a8) * 0.5f, cy = *reinterpret_cast<int*>(c + 0x1ac) * 0.5f;
    screen[0] = cx + (screen[0] - cx) * ratio;
    screen[1] = cy + (screen[1] - cy) * ratio;
    ++g_nw[5];
  }
  NameRec& r = g_names[g_name_obj];
  if (r.tick != tick) {
    r.prev[0] = r.cur[0];
    r.prev[1] = r.cur[1];
    r.prev_tick = r.tick;
    r.tick = tick;
  }
  r.cur[0] = screen[0];
  r.cur[1] = screen[1];
  if (r.prev_tick == 0 || r.prev_tick + 1 != tick) { ++g_nw[2]; return ok; }
  const float dx = r.cur[0] - r.prev[0], dy = r.cur[1] - r.prev[1];
  if (dx * dx + dy * dy > 400.0f * 400.0f) { ++g_nw[3]; return ok; }
  screen[0] = r.prev[0] + dx * alpha;
  screen[1] = r.prev[1] + dy * alpha;
  ++g_nw[1];
  if (!g_name_sample) {
    g_name_sample = true;
    patch::Log("smoothing: name %08x at (%.1f %.1f) was (%.1f %.1f), drawn at a=%.2f (%.1f %.1f)",
               static_cast<unsigned>(g_name_obj), r.cur[0], r.cur[1], r.prev[0], r.prev[1], alpha, screen[0],
               screen[1]);
  }
  return ok;
}

}  // namespace

#if defined(_MSC_VER)
__declspec(naked) void NameProjectThunk() {
  __asm {
    mov g_name_obj, esi
    jmp g_name_project_fn
  }
}
#else
extern "C" void NameProjectThunk();
asm(".text\n"
    ".globl _NameProjectThunk\n"
    "_NameProjectThunk:\n"
    "  movl %esi, _g_name_obj\n"
    "  jmp *_g_name_project_fn\n");
#endif

void InstallPartSmoothing() {
  static const uint8_t rebuild_entry[] = {0x83, 0xec, 0x20, 0x8b, 0x44, 0x24, 0x24};
  static const uint8_t xform_call[] = {0x55, 0xe8, 0x2c, 0x12, 0x06, 0x00};
  static const uint8_t draw_call[] = {0x8b, 0xcf, 0xe8, 0xe1, 0xce, 0xff, 0xff};
  static const uint8_t light_call[] = {0xe8, 0x25, 0xdc, 0x05, 0x00};
  if (!patch::Matches(0x441CE0, rebuild_entry, sizeof(rebuild_entry)) ||
      !patch::Matches(0x441E3E, xform_call, sizeof(xform_call)) ||
      !patch::Matches(0x444FE8, draw_call, sizeof(draw_call)) ||
      !patch::Matches(0x441E26, light_call, sizeof(light_call))) {
    patch::Log("smoothing: parts NOT hooked (unexpected bytes)");
    return;
  }
  // Trampoline: the 7 moved bytes (sub esp,20h; mov eax,[esp+24h]) then jmp back.
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) { patch::Log("smoothing: parts NOT hooked (no memory)"); return; }
  std::memcpy(t, rebuild_entry, sizeof(rebuild_entry));
  t[7] = 0xe9;
  const int32_t back = static_cast<int32_t>(0x441CE7 - reinterpret_cast<uintptr_t>(t + 12));
  std::memcpy(t + 8, &back, 4);
  FlushInstructionCache(GetCurrentProcess(), t, 16);
  g_rebuild = reinterpret_cast<RebuildFn>(t);
  patch::Jump(0x441CE0, reinterpret_cast<uintptr_t>(&RebuildHook), 7);
  patch::Call(0x441E3F, reinterpret_cast<void*>(&PartXform), 5);
  patch::Call(0x444FEA, reinterpret_cast<void*>(&PartDraw), 5);
  patch::Call(0x441E26, reinterpret_cast<void*>(&PartLight), 5);
  patch::Log("smoothing: bike and rider parts placed between steps before the game builds them");
}

void InstallDropTextSmoothing() {
  const uintptr_t slot = 0x55DA74 + 15 * 4;
  if (*reinterpret_cast<uintptr_t*>(slot) != 0x520740) {
    patch::Log("smoothing: drop text NOT hooked (slot holds %08x)", *reinterpret_cast<unsigned*>(slot));
    return;
  }
  void* fn = reinterpret_cast<void*>(&DropDraw);
  patch::Write(slot, &fn, sizeof(fn));
  patch::Log("smoothing: dropping text (FINAL LAP) placed between steps");
}

void InstallNameSmoothing() {
  static const uint8_t call[] = {0x52, 0x50, 0xe8, 0x2b, 0x60, 0x01, 0x00};
  if (!patch::Matches(0x51E97E, call, sizeof(call))) {
    patch::Log("smoothing: names NOT hooked (unexpected bytes)");
    return;
  }
  InstallDropTextSmoothing();
  g_name_project_fn = reinterpret_cast<void*>(&NameProject);
  patch::Call(0x51E980, reinterpret_cast<void*>(&NameProjectThunk), 5);
  patch::Log("smoothing: names over riders placed between steps");
}

void PartsFrameEnd(bool log) {
  ++g_part_frame;
  if (!log) return;
  unsigned tick;
  float alpha;
  const bool on = SimDrawPoint(&tick, &alpha);
  if (g_pw[0] || g_pw[7] || g_still_skipped) {
    patch::Log("smoothing: parts: %u object draws rebuilt, %u transforms + %u lightings; %u blended (most turn %.1f deg, %u "
               "the long way round); skipped: unchanged %u, no previous %u, jumped %u, bad matrix %u; %u tracked",
               g_pw[0], g_pw[7], g_pw[8], g_pw[1], g_max_turn * 180 / kPi, g_pw[6], g_pw[2], g_pw[3], g_pw[4],
               g_pw[5], static_cast<unsigned>(g_parts.size()));
    patch::Log("smoothing: parts: game rebuilds %u during steps, %u while drawing; %u draws left unblended by the vertex smoother; %u matrices changed within a frame; %u still-object draws left alone (%u objects seen moving)",
               g_pw[9], g_pw[10], g_pw[12], g_changed, g_still_skipped, static_cast<unsigned>(g_sim_rebuilt.size()));
  }
  g_changed = 0;
  if (g_nw[0]) {
    patch::Log("smoothing: names: %u placed, %u blended, %u moved for the cull margin; skipped: no previous %u, "
               "jumped %u, off screen %u; %u tracked; %u projections narrowed back",
               g_nw[0], g_nw[1], g_nw[5], g_nw[2], g_nw[3], g_nw[4], static_cast<unsigned>(g_names.size()),
               SmoothMarginHits());
  }
  if (g_dw[0]) {
    patch::Log("smoothing: drop text: %u draws, %u blended; skipped: no previous %u, jumped %u", g_dw[0], g_dw[1],
               g_dw[2], g_dw[3]);
  }
  for (unsigned& w : g_dw) w = 0;
  g_drop_sample = false;
  if (!on) g_drops.clear();
  for (unsigned& w : g_pw) w = 0;
  for (unsigned& w : g_nw) w = 0;
  g_max_turn = 0.0f;
  g_part_sample = g_name_sample = false;
  for (auto it = g_parts.begin(); it != g_parts.end();) {
    if (!on || it->second.tick + 30 < tick) it = g_parts.erase(it);
    else ++it;
  }
  for (auto it = g_names.begin(); it != g_names.end();) {
    if (!on || it->second.tick + 30 < tick) it = g_names.erase(it);
    else ++it;
  }
}
