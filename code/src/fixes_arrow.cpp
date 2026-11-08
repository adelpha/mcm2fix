// Race direction arrow ("VisualCue", viscue.slt).
//
// The green 3D arrow in the bottom-left corner is a real object in the 3D
// scene, placed every frame in front of the camera (VisualCue update,
// 0x48c0a0): position = camera + forward * dist(+0x44) + right * h(+0xb4)
// + up * v(+0xb8), where h and v put it at a fixed screen spot. It is then
// aimed with SetOrientation (0x401520) along (target - camera position).
//
// That direction is right in world space, but the arrow sits far off the
// view axis, so perspective bends it: an arrow pointing straight ahead is
// drawn leaning toward the vanishing point in the middle of the screen,
// i.e. up and to the right. That is why it points to the right of a gate
// you are driving straight at. We turn the direction about the camera's up
// axis by the angle between the view axis and the camera-to-arrow ray, so
// "straight ahead" is drawn straight up the screen, and optionally scale
// the arrow up (it was sized for 640x480).
#include <windows.h>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kSetOrientation = 0x401520;
constexpr uintptr_t kSetOrientationResume = 0x401528;
const uint8_t kSetOrientationEntry[] = {0x8B, 0x44, 0x24, 0x10,   // mov eax,[esp+10h]
                                        0x8B, 0x54, 0x24, 0x0C};  // mov edx,[esp+0Ch]
constexpr uint32_t kVisualCueVtable = 0x5593D8;

struct Vec {
  float x, y, z;
};
Vec Load(uintptr_t a) { return *reinterpret_cast<const Vec*>(a); }
float Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Scale(const Vec& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec Add(const Vec& a, const Vec& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
bool Normalize(Vec& a) {
  float l = std::sqrt(Dot(a, a));
  if (!(l > 1e-6f)) return false;
  a = Scale(a, 1.0f / l);
  return true;
}

struct ScaleState {
  float base;     // the game's own size
  float written;  // what we stored in its place
};
std::map<uintptr_t, ScaleState> g_scale;

void __stdcall AdjustCue(uintptr_t cue, float* dir) {
  if (*reinterpret_cast<uint32_t*>(cue) != kVisualCueVtable) return;
  const uintptr_t cam = *reinterpret_cast<uintptr_t*>(cue + 0x40);
  if (!cam) return;

  if (GetConfig().arrow_heading && dir) {
    // Camera basis (columns of its rotation matrix, see 0x48c14c).
    Vec right = {*reinterpret_cast<float*>(cam + 0xac), *reinterpret_cast<float*>(cam + 0xbc),
                 *reinterpret_cast<float*>(cam + 0xcc)};
    Vec up = {*reinterpret_cast<float*>(cam + 0xb0), *reinterpret_cast<float*>(cam + 0xc0),
              *reinterpret_cast<float*>(cam + 0xd0)};
    Vec fwd = {*reinterpret_cast<float*>(cam + 0xb4), *reinterpret_cast<float*>(cam + 0xc4),
               *reinterpret_cast<float*>(cam + 0xd4)};
    const float dist = *reinterpret_cast<float*>(cue + 0x44);
    const float h = *reinterpret_cast<float*>(cue + 0xb4);
    if (Normalize(right) && Normalize(up) && Normalize(fwd) && dist > 0.0f && std::isfinite(h)) {
      // view axis -> arrow, about camera up; ArrowTurn scales it (1 = full).
      const float a = std::atan2(h, dist) * GetConfig().arrow_turn;
      const float c = std::cos(a), s = std::sin(a);
      Vec d = Load(reinterpret_cast<uintptr_t>(dir));
      const float x = Dot(d, right), y = Dot(d, up), z = Dot(d, fwd);
      const float x2 = x * c + z * s;
      float z2 = z * c - x * s;
      float x3 = x2, y2 = y;
      // ArrowTilt: raise the nose a few degrees so the arrow is seen a
      // little more from above.
      const float tilt = GetConfig().arrow_tilt * 3.14159265f / 180.0f;
      const float flat = std::sqrt(x2 * x2 + z2 * z2);
      if (tilt != 0.0f && flat > 1e-6f) {
        const float len = std::sqrt(flat * flat + y * y);
        float e = std::atan2(y, flat) + tilt;
        if (e > 1.4f) e = 1.4f;
        if (e < -1.4f) e = -1.4f;
        const float k2 = std::cos(e) * len / flat;
        x3 = x2 * k2;
        z2 = z2 * k2;
        y2 = std::sin(e) * len;
      }
      Vec out = Add(Add(Scale(right, x3), Scale(up, y2)), Scale(fwd, z2));
      if (std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z)) {
        dir[0] = out.x;
        dir[1] = out.y;
        dir[2] = out.z;
      }
    }
  }

  // Size: +0x34 is recomputed by the game only when the field of view
  // changes, so remember the game's value and keep ours on top of it.
  const float k = GetConfig().arrow_scale;
  float& size = *reinterpret_cast<float*>(cue + 0x34);
  if (!(size > 0.0f) || !std::isfinite(size)) return;
  float base = size;
  auto it = g_scale.find(cue);
  if (it != g_scale.end() && size == it->second.written) base = it->second.base;
  size = base * k;
  g_scale[cue] = {base, size};
}

}  // namespace

void ApplyArrowFix() {
  const Config& c = GetConfig();
  if (!c.arrow_heading && c.arrow_scale == 1.0f) return;
  if (!patch::Matches(kSetOrientation, kSetOrientationEntry, sizeof(kSetOrientationEntry))) {
    patch::Log("direction arrow: code differs, not applied");
    return;
  }
  // Thunk: if this is a VisualCue, AdjustCue(ecx, dir); then the replaced
  // instructions and back.
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  size_t n = 0;
  auto put = [&](std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) t[n++] = x;
  };
  auto put32 = [&](uint32_t v) {
    std::memcpy(t + n, &v, 4);
    n += 4;
  };
  put({0x81, 0x39});
  put32(kVisualCueVtable);  // cmp dword [ecx], VisualCue vtable
  put({0x75, 0x0E});        // jne +14 (skip to the original code)
  put({0x60});              // pushad
  put({0xFF, 0x74, 0x24, 0x24});  // push [esp+24h] (dir: arg 1 after pushad)
  put({0x51});                    // push ecx
  put({0xB8});
  put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&AdjustCue)));  // mov eax, AdjustCue
  put({0xFF, 0xD0});  // call eax
  put({0x61});        // popad
  // The jne above lands here (1+4+1+5+2+1 = 14 bytes after it).
  for (uint8_t b : kSetOrientationEntry) put({b});
  put({0x68});
  put32(static_cast<uint32_t>(kSetOrientationResume));  // push resume
  put({0xC3});                                          // ret
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(kSetOrientation, reinterpret_cast<uintptr_t>(t), sizeof(kSetOrientationEntry));
  patch::Log("direction arrow: applied (heading=%d turn=%.2f tilt=%.1f scale=%.2f)",
             c.arrow_heading ? 1 : 0, static_cast<double>(c.arrow_turn),
             static_cast<double>(c.arrow_tilt), static_cast<double>(c.arrow_scale));
}
