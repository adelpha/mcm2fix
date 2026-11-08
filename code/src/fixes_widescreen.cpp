// Widescreen field of view.
//
// The game's camera keeps a horizontal field of view in degrees at
// camera+0x16c (77 by default, adjustable with the zoom keys) and the view
// size at +0x1a8 / +0x1ac. Its projection, frustum culling and 2D label
// placement all derive from those, keeping the horizontal angle fixed, so
// a wide mode crops the top and bottom ("vert-"). We hook the camera update
// (0x42fd50) and widen the horizontal angle so the vertical angle matches
// what 4:3 would show. Everything downstream then agrees.
#include <windows.h>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kCameraUpdate = 0x42FD50;
constexpr uintptr_t kCameraUpdateResume = 0x42FD56;
const uint8_t kCameraUpdateEntry[] = {0x83, 0xEC, 0x48, 0x53, 0x8B, 0xD9};  // sub esp,48h; push ebx; mov ebx,ecx

struct FovState {
  float base;     // the game's own (4:3) value
  float written;  // what we stored in its place
};
std::map<uintptr_t, FovState> g_fov;

float Widen(float deg, float aspect) {
  const float kPi = 3.14159265f;
  float half = std::tan(deg * 0.5f * kPi / 180.0f) * aspect * 0.75f;
  float out = 2.0f * std::atan(half) * 180.0f / kPi;
  return out > 170.0f ? 170.0f : out;
}

void __stdcall AdjustFov(uintptr_t cam) {
  int w = *reinterpret_cast<int*>(cam + 0x1a8);
  int h = *reinterpret_cast<int*>(cam + 0x1ac);
  if (w <= 0 || h <= 0) return;
  float& fov = *reinterpret_cast<float*>(cam + 0x16c);
  if (!(fov > 1.0f && fov < 179.0f)) return;
  float base = fov;
  auto it = g_fov.find(cam);
  if (it != g_fov.end()) {
    float delta = fov - it->second.written;
    if (delta == 0.0f) {
      base = it->second.base;
    } else if (std::fabs(delta) <= 1.5f) {
      base = it->second.base + delta;  // zoom keys step our widened value
    }
  }
  const float aspect = static_cast<float>(w) / static_cast<float>(h);
  const float target = aspect > 4.0f / 3.0f + 0.01f ? Widen(base, aspect) : base;
  fov = target;
  g_fov[cam] = {base, target};
}

// The speedometer (InstrumentOverlay, instr1.tga) lays itself out in
// 640x480 units scaled by the view width and height separately (0x51e06b),
// so on a wide screen it is stretched sideways. Right after the layout
// (call at 0x51e1aa) we narrow its rectangle and needle length back to a
// 4:3 shape, keeping it the same distance from its screen edge.
constexpr uintptr_t kInstrumentLayoutCall = 0x51E1AA;
constexpr uintptr_t kInstrumentBuild = 0x51E2E0;

void __stdcall AdjustInstrument(uintptr_t inst) {
  uintptr_t owner = *reinterpret_cast<uintptr_t*>(inst + 0x18);
  if (!owner) return;
  uintptr_t cam = *reinterpret_cast<uintptr_t*>(owner + 8);
  if (!cam) return;
  const int vx = *reinterpret_cast<int*>(cam + 0x1a0);
  const int w = *reinterpret_cast<int*>(cam + 0x1a8);
  const int h = *reinterpret_cast<int*>(cam + 0x1ac);
  if (w <= 0 || h <= 0 || w * 3 <= h * 4 + 4) return;
  const float s = (h * 4.0f / 3.0f) / w;
  int& left = *reinterpret_cast<int*>(inst + 0x11c);
  int& right = *reinterpret_cast<int*>(inst + 0x124);
  float& needle = *reinterpret_cast<float*>(inst + 0x12c);
  const int width = static_cast<int>((right - left) * s + 0.5f);
  if (left + right > 2 * vx + w) {  // right half: keep the right margin (scaled)
    const int margin = vx + w - right;
    right = vx + w - static_cast<int>(margin * s + 0.5f);
    left = right - width;
  } else {
    const int margin = left - vx;
    left = vx + static_cast<int>(margin * s + 0.5f);
    right = left + width;
  }
  needle *= s;
}

}  // namespace

void ApplyWidescreenFov() {
  if (!GetConfig().wide_fov) return;
  if (!patch::Matches(kCameraUpdate, kCameraUpdateEntry, sizeof(kCameraUpdateEntry))) {
    patch::Log("widescreen fov: camera update code differs, not applied");
    return;
  }
  // Thunk: save ecx, AdjustFov(ecx), restore, run the replaced instructions,
  // jump back.
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
  put({0x51, 0x51});  // push ecx; push ecx (argument)
  put({0xB8});
  put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&AdjustFov)));  // mov eax, AdjustFov
  put({0xFF, 0xD0});  // call eax
  put({0x59});        // pop ecx
  for (uint8_t b : kCameraUpdateEntry) put({b});
  put({0x68});
  put32(static_cast<uint32_t>(kCameraUpdateResume));  // push resume
  put({0xC3});                                        // ret
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(kCameraUpdate, reinterpret_cast<uintptr_t>(t), sizeof(kCameraUpdateEntry));
  patch::Log("widescreen fov: applied");

  // Speedometer: call our adjustment, then the original layout builder.
  const uint8_t call_build[] = {0xE8, 0x31, 0x01, 0x00, 0x00};  // call 0x51e2e0
  if (!patch::Matches(kInstrumentLayoutCall, call_build, sizeof(call_build))) {
    patch::Log("widescreen speedometer: code differs, not applied");
    return;
  }
  auto* u = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!u) return;
  size_t m = 0;
  auto emit = [&](std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) u[m++] = x;
  };
  auto emit32 = [&](uint32_t v) {
    std::memcpy(u + m, &v, 4);
    m += 4;
  };
  emit({0x60});        // pushad
  emit({0x56});        // push esi (the overlay)
  emit({0xB8});
  emit32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&AdjustInstrument)));
  emit({0xFF, 0xD0});  // call eax
  emit({0x61});        // popad
  emit({0x68});
  emit32(static_cast<uint32_t>(kInstrumentBuild));  // push 0x51e2e0
  emit({0xC3});                                     // ret (= jmp, caller's return address stays)
  FlushInstructionCache(GetCurrentProcess(), u, m);
  patch::Call(kInstrumentLayoutCall, u, sizeof(call_build));
  patch::Log("widescreen speedometer: applied");
}
