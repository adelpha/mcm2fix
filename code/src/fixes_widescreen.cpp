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

// The camera's field of view (+0x16c) holds our widened (and, while
// smoothing, culling-margin) value from the camera update on; the sky, fog
// and everything else drawn from the camera read it there. The game's own
// bike camera code also reads +0x16c back (its zoom smoothing seed, its zoom
// target, its fov/max-fov ratios); those reads are redirected to GameFov(),
// which gives the game's own value. Without that, a camera that was already
// widened (the second race of a session) seeded the zoom smoothing with our
// value, and the start and podium zooms jumped and the arrow was misplaced.
struct FovState {
  float base;     // the game's own (4:3) value, after the ini Fov
  float drawn;    // the field of view the view is drawn with (widened, no margin)
  float written;  // what the projection was built with (drawn + culling margin)
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
// FovLog: last logged state per camera.
struct FovLogState {
  bool seen;
  float in, written, ratio;
};
std::map<uintptr_t, FovLogState> g_fov_log;
unsigned g_fov_lines = 0;
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
  const float in = fov;
  float base = fov;
  bool fresh = true;
  auto it = g_fov.find(cam);
  const bool known = it != g_fov.end();
  const char* why = "new camera";
  if (known) {
    float delta = fov - it->second.written;
    why = "game set it";
    if (delta == 0.0f) {
      base = it->second.base;
      fresh = false;
      why = "unchanged";
    } else if (std::fabs(delta) <= 1.5f) {
      base = it->second.base + delta;  // zoom keys step our widened value
      fresh = false;
      why = "zoom key step";
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
  if (cfg.fov_log) {
    // One line whenever something about this camera's field of view changes.
    FovLogState& L = g_fov_log[cam];
    const float ratio = g_margin.count(cam) ? g_margin[cam].ratio : 1.0f;
    if (!known) g_fov_lines = 0;  // a new camera (race load): log it afresh
    if (!L.seen || in != L.in || written != L.written || ratio != L.ratio) {
      if (g_fov_lines < 20000) {
        ++g_fov_lines;
        patch::Log("fov: cam %08x %dx%d game %.3f -> base %.3f (%s) drawn %.3f culls %.3f margin %.3f%s",
                   static_cast<unsigned>(cam), w, h, in, base,
                   why, target, written, ratio,
                   SimDrawPoint(&tick, &alpha) ? " racing" : "");
      }
      L = {true, in, written, ratio};
    }
  }
  fov = written;
  g_fov[cam] = {base, target, written};
}

// The game's own field of view for a camera: our base while +0x16c still
// holds what we wrote, otherwise whatever the game stored there since.
float __stdcall GameFov(uintptr_t cam) {
  const float fov = *reinterpret_cast<float*>(cam + 0x16c);
  auto it = g_fov.find(cam);
  if (it != g_fov.end() && fov == it->second.written) return it->second.base;
  return fov;
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

// FovLog: the game's own field-of-view setter (0x42f850, thiscall, one
// float). Logged with its caller so we can tell the bike camera's zoom
// smoothing, the zoom keys and camera mode switches apart.
constexpr uintptr_t kSetFov = 0x42F850;
const uint8_t kSetFovEntry[] = {0xD9, 0x44, 0x24, 0x04,               // fld dword [esp+4]
                                0xDC, 0x1D, 0xA0, 0x58, 0x55, 0x00};  // fcomp qword [5558a0h]
struct SetFovLog {
  float last = 0.0f;
  uintptr_t from = 0;
  unsigned same = 0;
};
std::map<uintptr_t, SetFovLog> g_setfov;

void __stdcall LogSetFov(uintptr_t cam, uintptr_t from, float value) {
  SetFovLog& L = g_setfov[cam];
  if (value == L.last && from == L.from) {
    ++L.same;
    return;
  }
  if (g_fov_lines < 20000) {
    ++g_fov_lines;
    const auto it = g_fov.find(cam);
    patch::Log("fov: SetFov cam %08x from %08x value %.3f (camera holds %.3f, our base %.3f drawn-with-margin %.3f; "
               "%u repeats of the last call)",
               static_cast<unsigned>(cam), static_cast<unsigned>(from), value, *reinterpret_cast<float*>(cam + 0x16c),
               it != g_fov.end() ? it->second.base : 0.0f, it != g_fov.end() ? it->second.written : 0.0f, L.same);
  }
  L = {value, from, 0};
}

void InstallSetFovLog() {
  if (!patch::Matches(kSetFov, kSetFovEntry, sizeof(kSetFovEntry))) {
    patch::Log("fov log: SetFov code differs, not hooked");
    return;
  }
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  size_t n = 0;
  auto put = [&](std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) t[n++] = x;
  };
  auto put32 = [&](uint32_t v) {
    std::memcpy(t + n, &v, 4);
    n += 4;
  };
  put({0x60});                    // pushad
  put({0xFF, 0x74, 0x24, 0x24});  // push [esp+24h]  (value)
  put({0xFF, 0x74, 0x24, 0x24});  // push [esp+24h]  (return address, now one slot further)
  put({0x51});                    // push ecx        (camera)
  put({0xB8});
  put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&LogSetFov)));
  put({0xFF, 0xD0});  // call eax
  put({0x61});        // popad
  for (uint8_t b : kSetFovEntry) put({b});
  put({0x68});
  put32(static_cast<uint32_t>(kSetFov + sizeof(kSetFovEntry)));
  put({0xC3});
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(kSetFov, reinterpret_cast<uintptr_t>(t), sizeof(kSetFovEntry));
  patch::Log("fov log: SetFov 0x42f850 logged");
}

// The game's camera code that reads +0x16c back gets GameFov() instead.
// Each `flds 0x16c(reg)` (6 bytes) becomes a call to a small stub that loads
// GameFov(reg) onto the FPU stack, keeping the general registers and flags.
struct FovRead {
  uintptr_t at;
  uint8_t modrm;  // 0x86 esi, 0x81 ecx, 0x80 eax
  uint8_t reg_push;  // push reg
  const char* what;
};
const FovRead kFovReads[] = {
    {0x4640A6, 0x86, 0x56, "bike camera zoom smoothing seed"},
    {0x417FB4, 0x86, 0x56, "fov / max fov ratio"},
    {0x49948B, 0x86, 0x56, "fov / max fov ratio"},
    {0x4990E1, 0x81, 0x51, "fov / max fov ratio"},
    {0x48DDEC, 0x80, 0x50, "fov vs max fov"},
};
// Bike camera zoom pull (0x4671a6..0x4671c3):
//   target(+0x258) = fov + k * (target - fov), then jmp 0x4672d1.
// k's address is read from the fmuls operand at 0x4671b4 each time, because
// FrameFix redirects it (per-step rate) after we are installed.
constexpr uintptr_t kZoomPull = 0x4671A6;
constexpr uintptr_t kZoomPullOperand = 0x4671B4;
constexpr uintptr_t kZoomPullDone = 0x4672D1;
const uint8_t kZoomPullCode[] = {0xD9, 0x86, 0x58, 0x02, 0x00, 0x00,   // flds 0x258(%esi)
                                 0xD8, 0xA6, 0x6C, 0x01, 0x00, 0x00,   // fsubs 0x16c(%esi)
                                 0xD8, 0x0D};                          // fmuls [k]

void InstallGameFovReads() {
  int done = 0;
  for (const FovRead& r : kFovReads) {
    const uint8_t code[] = {0xD9, r.modrm, 0x6C, 0x01, 0x00, 0x00};
    if (!patch::Matches(r.at, code, sizeof(code))) {
      patch::Log("widescreen fov: fov read at %08x (%s) differs, left alone", static_cast<unsigned>(r.at), r.what);
      continue;
    }
    Thunk s;
    if (!s.p) return;
    s.put({0x9C, 0x50, 0x51, 0x52});  // pushfd; push eax; push ecx; push edx
    s.put({r.reg_push});              // push reg (the camera)
    s.put({0xB8});
    s.put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&GameFov)));
    s.put({0xFF, 0xD0});                    // call eax (st0 = game fov)
    s.put({0x5A, 0x59, 0x58, 0x9D, 0xC3});  // pop edx; pop ecx; pop eax; popfd; ret
    s.done();
    patch::Call(r.at, s.p, sizeof(code));
    ++done;
  }
  if (patch::Matches(kZoomPull, kZoomPullCode, sizeof(kZoomPullCode))) {
    Thunk z;
    if (!z.p) return;
    z.put({0x60, 0x56});  // pushad; push esi
    z.put({0xB8});
    z.put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&GameFov)));
    z.put({0xFF, 0xD0});                    // call eax: st0 = g
    z.put({0x61});                          // popad
    z.put({0xD9, 0x86, 0x58, 0x02, 0x00, 0x00});  // flds 0x258(%esi): st0 = target, st1 = g
    z.put({0xD8, 0xE1});                    // fsub st0, st1: target - g
    z.put({0x50, 0xA1});                    // push eax; mov eax, [operand]
    z.put32(static_cast<uint32_t>(kZoomPullOperand));
    z.put({0xD8, 0x08});                    // fmul dword [eax]
    z.put({0x58});                          // pop eax
    z.put({0xDE, 0xC1});                    // faddp st1, st0: g + k*(target - g)
    z.put({0xD9, 0x9E, 0x58, 0x02, 0x00, 0x00});  // fstps 0x258(%esi)
    z.put({0x68});
    z.put32(static_cast<uint32_t>(kZoomPullDone));
    z.put({0xC3});  // jmp 0x4672d1
    z.done();
    patch::Jump(kZoomPull, reinterpret_cast<uintptr_t>(z.p), 5);
    ++done;
  } else {
    patch::Log("widescreen fov: bike camera zoom pull code differs, left alone");
  }
  patch::Log("widescreen fov: %d game reads of the camera fov see the game's own value", done);
}

}  // namespace

float SmoothProjectionScale(const float* m) {
  // Recognised by shape (width to height), not by value: the game zooms the
  // camera at a race start (supercross gates), so the projection being drawn
  // is often not the one seen at the last update, and drawing it unnarrowed
  // made the view jump in and out by the margin.
  if (m[11] == 0.0f || m[0] == 0.0f) return 1.0f;
  for (const auto& c : g_margin)
    if (c.second.p00 != 0.0f && std::fabs(m[5] / m[0] - c.second.p11 / c.second.p00) < 1e-3f * (c.second.p11 / c.second.p00)) {
      ++g_margin_hits;
      return c.second.ratio;
    }
  return 1.0f;
}

bool CameraDrawnFov(uintptr_t cam, float* drawn) {
  auto it = g_fov.find(cam);
  if (it == g_fov.end()) return false;
  *drawn = it->second.drawn;
  return true;
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
  if (GetConfig().fov_log) InstallSetFovLog();
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
  InstallGameFovReads();
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
