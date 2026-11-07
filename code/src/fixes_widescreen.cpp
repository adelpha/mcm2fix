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
}
