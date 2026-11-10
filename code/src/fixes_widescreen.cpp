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
#include "framerate.h"
#include "smooth.h"
#include "patch.h"
#include "dd/hudtext.h"

namespace {

constexpr uintptr_t kCameraUpdate = 0x42FD50;
constexpr uintptr_t kCameraUpdateResume = 0x42FD56;
const uint8_t kCameraUpdateEntry[] = {0x83, 0xEC, 0x48, 0x53, 0x8B, 0xD9};  // sub esp,48h; push ebx; mov ebx,ecx

constexpr float kGameFov = 77.0f;  // the game's default camera field of view

struct FovState {
  float base;     // the game's own (4:3) value
  float written;  // what we stored in its place
};
std::map<uintptr_t, FovState> g_fov;

// Smoothing draws the camera up to one simulation step behind the one the
// game culls with, so when turning fast, things at the screen edge were
// already culled. While smoothing, the game's camera gets a wider angle
// (more is kept) and the drawing projection and the names are scaled back.
constexpr float kCullMargin = 1.2f;  // ratio of tan(half angle)
struct MarginCam {
  float ratio = 1.0f;
  float p00 = 0.0f, p11 = 0.0f;  // the camera's projection after the last update, to recognise it
};
std::map<uintptr_t, MarginCam> g_margin;
unsigned g_margin_hits = 0;
bool g_margin_logged = false;

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
  const Config& cfg = GetConfig();
  float base = fov;
  bool fresh = true;
  auto it = g_fov.find(cam);
  if (it != g_fov.end()) {
    float delta = fov - it->second.written;
    if (delta == 0.0f) {
      base = it->second.base;
      fresh = false;
    } else if (std::fabs(delta) <= 1.5f) {
      base = it->second.base + delta;  // zoom keys step our widened value
      fresh = false;
    }
  }
  // ini Fov replaces the game's default field of view on new cameras
  // (the zoom keys still step from there).
  if (fresh && cfg.fov > 0.0f && std::fabs(base - kGameFov) < 0.01f) base = cfg.fov;
  const float aspect = static_cast<float>(w) / static_cast<float>(h);
  const bool widen = cfg.wide_fov && aspect > 4.0f / 3.0f + 0.01f;
  const float target = widen ? Widen(base, aspect) : base;
  float written = target;
  unsigned tick;
  float alpha;
  if (w >= 320 && aspect >= 1.3f && SimDrawPoint(&tick, &alpha)) {  // the main view, not the square shadow camera
    const float kPi = 3.14159265f;
    float wide = 2.0f * std::atan(std::tan(target * 0.5f * kPi / 180.0f) * kCullMargin) * 180.0f / kPi;
    if (wide > 170.0f) wide = 170.0f;
    MarginCam& mc = g_margin[cam];
    mc.ratio = std::tan(wide * 0.5f * kPi / 180.0f) / std::tan(target * 0.5f * kPi / 180.0f);
    mc.p00 = *reinterpret_cast<float*>(cam + 0x6c);
    mc.p11 = *reinterpret_cast<float*>(cam + 0x6c + 20);
    written = wide;
    if (!g_margin_logged) {
      g_margin_logged = true;
      patch::Log("smoothing: camera %08x culls with %.1f deg, drawn at %.1f (%dx%d)", static_cast<unsigned>(cam),
                 wide, target, w, h);
    }
  } else {
    g_margin.erase(cam);
  }
  fov = written;
  g_fov[cam] = {base, written};
}

// The speedometer (InstrumentOverlay, instr1.tga, vtable 0x55d914) is a
// screen overlay laid out in 640x480 units, scaled by the view width and
// height separately, so on a wide screen it is stretched sideways.
//
// The dial quad comes from the overlay base layout (0x4baad0), which maps
// the rectangle at +0xe8 (x1, y1, x2, y2 in 640x480 units). Before it runs
// we narrow that rectangle to a 4:3 shape, keeping its distance from the
// nearest screen edge in proportion. The needle is laid out separately
// (0x51e06b): its pivot starts from the same +0xe8 corner, then adds an
// offset and a length that also scale with the view width; right after
// that (call at 0x51e1aa) we scale those two by the same factor.
constexpr uintptr_t kOverlayLayout = 0x4BAAD0;
constexpr uintptr_t kOverlayLayoutResume = 0x4BAAD5;
const uint8_t kOverlayLayoutEntry[] = {0x83, 0xEC, 0x3C, 0x53, 0x56};  // sub esp,3ch; push ebx; push esi
constexpr uintptr_t kInstrumentVtable = 0x55D914;
constexpr uintptr_t kInstrumentLayoutCall = 0x51E1AA;
constexpr uintptr_t kInstrumentBuild = 0x51E2E0;

// Narrowing factor for the overlay's view, or 1 when the view is not wide.
float OverlayScale(uintptr_t inst) {
  uintptr_t owner = *reinterpret_cast<uintptr_t*>(inst + 0x18);
  if (!owner) return 1.0f;
  int w = 0, h = 0;
  uintptr_t cam = *reinterpret_cast<uintptr_t*>(owner + 8);
  if (cam) {
    w = *reinterpret_cast<int*>(cam + 0x1a8);
    h = *reinterpret_cast<int*>(cam + 0x1ac);
  } else {
    w = *reinterpret_cast<int*>(owner + 0xc);
    h = *reinterpret_cast<int*>(owner + 0x10);
  }
  if (w <= 0 || h <= 0 || w * 3 <= h * 4 + 4) return 1.0f;
  return (h * 4.0f / 3.0f) / w;
}

struct RectState {
  int orig[2];     // the game's x1, x2
  int written[2];  // what we stored in their place
};
std::map<uintptr_t, RectState> g_rects;
bool g_logged_rect = false;

void __stdcall AdjustOverlayRect(uintptr_t inst) {
  if (*reinterpret_cast<uintptr_t*>(inst) != kInstrumentVtable) return;
  int& x1 = *reinterpret_cast<int*>(inst + 0xe8);
  int& x2 = *reinterpret_cast<int*>(inst + 0xf0);
  int ox1 = x1, ox2 = x2;
  auto it = g_rects.find(inst);
  if (it != g_rects.end() && x1 == it->second.written[0] && x2 == it->second.written[1]) {
    ox1 = it->second.orig[0];
    ox2 = it->second.orig[1];
  }
  const float s = OverlayScale(inst);
  int nx1 = ox1, nx2 = ox2;
  if (s < 1.0f) {
    const int width = static_cast<int>((ox2 - ox1) * s + 0.5f);
    if (ox1 + ox2 > 640) {  // right half: keep the right margin (scaled)
      nx2 = 640 - static_cast<int>((640 - ox2) * s + 0.5f);
      nx1 = nx2 - width;
    } else {
      nx1 = static_cast<int>(ox1 * s + 0.5f);
      nx2 = nx1 + width;
    }
  }
  x1 = nx1;
  x2 = nx2;
  g_rects[inst] = {{ox1, ox2}, {nx1, nx2}};
  if (!g_logged_rect) {
    g_logged_rect = true;
    patch::Log("widescreen speedometer: dial x %d..%d -> %d..%d (scale %.3f)", ox1, ox2, nx1,
               nx2, s);
  }
}

void __stdcall AdjustNeedle(uintptr_t inst) {
  const float s = OverlayScale(inst);
  if (s >= 1.0f) return;
  int& px = *reinterpret_cast<int*>(inst + 0x11c);
  int& cx = *reinterpret_cast<int*>(inst + 0x124);
  float& needle = *reinterpret_cast<float*>(inst + 0x12c);
  cx = px + static_cast<int>((cx - px) * s + 0.5f);
  needle *= s;
}

bool g_layout_hooked = false;
bool g_wide_dial = false;

// Every screen overlay's layout (0x4baad0) starts here.
void __stdcall OverlayLayoutHook(uintptr_t inst) {
  if (g_wide_dial) AdjustOverlayRect(inst);
  dd::HudPanelLayout(inst);
}

// Allocates a small executable thunk.
struct Thunk {
  uint8_t* p = nullptr;
  size_t n = 0;
  Thunk() {
    p = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  }
  void put(std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) p[n++] = x;
  }
  void put32(uint32_t v) {
    std::memcpy(p + n, &v, 4);
    n += 4;
  }
  void done() { FlushInstructionCache(GetCurrentProcess(), p, n); }
};

}  // namespace

float SmoothProjectionScale(const float* m) {
  for (const auto& c : g_margin)
    if (c.second.p00 != 0.0f && m[0] == c.second.p00 && m[5] == c.second.p11) {
      ++g_margin_hits;
      return c.second.ratio;
    }
  return 1.0f;
}

bool SmoothCullMargin(uintptr_t cam, float* ratio) {
  auto it = g_margin.find(cam);
  if (it == g_margin.end()) return false;
  *ratio = it->second.ratio;
  return true;
}

unsigned SmoothMarginHits() {
  const unsigned n = g_margin_hits;
  g_margin_hits = 0;
  return n;
}

void InstallOverlayLayoutHook() {
  if (g_layout_hooked) return;
  if (!patch::Matches(kOverlayLayout, kOverlayLayoutEntry, sizeof(kOverlayLayoutEntry))) {
    patch::Log("overlay layout hook: code differs, not applied");
    return;
  }
  Thunk d;
  if (!d.p) return;
  d.put({0x60, 0x51});  // pushad; push ecx (the overlay)
  d.put({0xB8});
  d.put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OverlayLayoutHook)));
  d.put({0xFF, 0xD0, 0x61});  // call eax; popad
  for (uint8_t b : kOverlayLayoutEntry) d.put({b});
  d.put({0x68});
  d.put32(static_cast<uint32_t>(kOverlayLayoutResume));
  d.put({0xC3});
  d.done();
  patch::Jump(kOverlayLayout, reinterpret_cast<uintptr_t>(d.p), sizeof(kOverlayLayoutEntry));
  g_layout_hooked = true;
  patch::Log("overlay layout hook: applied");
}

void ApplyWidescreenFov() {
  InstallOverlayLayoutHook();
  if (!GetConfig().wide_fov && GetConfig().fov <= 0.0f) return;
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
  patch::Log("widescreen fov: applied (Fov %.1f, WideFov %d)", GetConfig().fov,
             GetConfig().wide_fov ? 1 : 0);
  if (!GetConfig().wide_fov) return;

  // Speedometer dial: AdjustOverlayRect, from the overlay layout hook.
  if (!g_layout_hooked) {
    patch::Log("widescreen speedometer: overlay layout code differs, not applied");
    return;
  }
  const uint8_t call_build[] = {0xE8, 0x31, 0x01, 0x00, 0x00};  // call 0x51e2e0
  if (!patch::Matches(kInstrumentLayoutCall, call_build, sizeof(call_build))) {
    patch::Log("widescreen speedometer: needle code differs, not applied");
    return;
  }
  g_wide_dial = true;

  // Speedometer needle: AdjustNeedle(esi), then the original vertex builder.
  Thunk u;
  if (!u.p) return;
  u.put({0x60, 0x56});  // pushad; push esi (the overlay)
  u.put({0xB8});
  u.put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&AdjustNeedle)));
  u.put({0xFF, 0xD0, 0x61});  // call eax; popad
  u.put({0x68});
  u.put32(static_cast<uint32_t>(kInstrumentBuild));
  u.put({0xC3});  // ret (= jmp, caller's return address stays)
  u.done();

  patch::Call(kInstrumentLayoutCall, u.p, sizeof(call_build));
  patch::Log("widescreen speedometer: applied");
}
