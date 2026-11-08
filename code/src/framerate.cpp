// Frame pacing: the frame-rate cap and a precise frame time for the game.
//
// The game's main tick (Game::Tick, 0x469510) measures the time since the
// last frame with timeGetTime in whole milliseconds (0x4c41d0), so at 144
// frames a second its step alternates between 6 and 7 ms (+-15%) and above
// 1000 it can be 0. We replace that measurement with one from
// QueryPerformanceCounter, keeping the game's own reset logic and 0.2 s cap.
//
// Most of the simulation scales by that frame time, but a few spots apply a
// fixed amount once per frame, so they run faster the higher the frame rate
// (see FixSites below). For each, we point the instruction's constant
// operand at a value of our own, recomputed every tick so that it does per
// frame what the original constant did per frame at RefRate (30 by default).
#include "framerate.h"
#include "smooth.h"

#include <windows.h>
#include <cmath>
#include <cstring>
#include <map>

#include "config.h"
#include "patch.h"

namespace {

LARGE_INTEGER g_freq{};
LONGLONG g_last_tick = 0;  // QPC of the game's previous tick
LONGLONG g_next_frame = 0; // QPC when the frame cap lets the next frame out
HANDLE g_timer = nullptr;

LONGLONG Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

// ---------------------------------------------------------------- per-frame constants

enum class Kind {
  kStep,  // added/compared once per frame: scale linearly, k * frames
  kDamp,  // multiplied once per frame: scale exponentially, k ^ frames
};

struct FixValue {
  float k;
  Kind kind;
  float value;  // what the patched instructions read
};

// x87 "op dword ptr [abs32]" with its opcode bytes, at a known address.
struct FixSite {
  uintptr_t addr;
  uint8_t op0, op1;  // D8 05 fadd, D8 25 fsub, D8 0D fmul, D8 1D fcomp
  uintptr_t constant;
  const char* what;
};

FixValue g_values[] = {
    {0.125f, Kind::kStep, 0.125f},  // 0: rider steering animation step (0x5558c4)
    {0.25f, Kind::kStep, 0.25f},    // 1: rider lean animation step (0x5558b0)
    {0.9f, Kind::kDamp, 0.9f},      // 2: spin damping in the air (0x5558d8)
    {0.999f, Kind::kDamp, 0.999f},  // 3: rigid body spin damping (0x5597fc)
    {0.5f, Kind::kDamp, 0.5f},      // 4: settling body spin damping (0x5558ac)
    {0.95f, Kind::kDamp, 0.95f},    // 5: settling body damping (0x55d0c0)
    {0.85f, Kind::kDamp, 0.85f},    // 6: scripted bike spin damping (0x55dfa4)
    {0.75f, Kind::kDamp, 0.75f},    // 7: camera zoom smoothing (0x555904)
};

constexpr uint8_t kFadd = 0x05, kFsub = 0x25, kFmul = 0x0D, kFcomp = 0x1D;

struct SiteGroup {
  int value;  // index into g_values
  FixSite sites[9];
};

const SiteGroup kGroups[] = {
    // Bike::Simulate (0x414000): the steering / lean values the rider
    // animation follows (+0x504 / +0x508) chase the physics by at most
    // 0.125 / 0.25 per frame. At 144 fps opponents' riders twitch and the
    // player's rider snaps upright the moment a turn is released.
    {0, {{0x414A22, 0xD8, kFcomp, 0x5558C4, "steer step compare"},
         {0x414A48, 0xD8, kFadd, 0x5558C4, "steer step +"},
         {0x414A50, 0xD8, kFsub, 0x5558C4, "steer step -"}}},
    {1, {{0x414A77, 0xD8, kFcomp, 0x5558B0, "lean step compare"},
         {0x414A97, 0xD8, kFadd, 0x5558B0, "lean step +"},
         {0x414AA5, 0xD8, kFsub, 0x5558B0, "lean step -"}}},
    // Bike air control (0x40bcc0): spin velocity *0.9 per frame, so at high
    // frame rates jumps lose their rotation much sooner; same for the
    // wobble amplitude at +0x71c.
    {2, {{0x40C0D8, 0xD8, kFmul, 0x5558D8, "air spin x"},
         {0x40C0EA, 0xD8, kFmul, 0x5558D8, "air spin y"},
         {0x40C0FC, 0xD8, kFmul, 0x5558D8, "air spin z"},
         {0x40C1A8, 0xD8, kFmul, 0x5558D8, "wobble"}}},
    // Rigid body integration (0x506ed0, KrustyBike 0x497590): spin *0.999
    // per frame.
    {3, {{0x49764A, 0xD8, kFmul, 0x5597FC, "bike spin x"},
         {0x49765C, 0xD8, kFmul, 0x5597FC, "bike spin y"},
         {0x49766E, 0xD8, kFmul, 0x5597FC, "bike spin z"},
         {0x506F5D, 0xD8, kFmul, 0x5597FC, "body spin x"},
         {0x506F6F, 0xD8, kFmul, 0x5597FC, "body spin y"},
         {0x506F81, 0xD8, kFmul, 0x5597FC, "body spin z"}}},
    // Bodies coming to rest (0x507a90).
    {4, {{0x507C7E, 0xD8, kFmul, 0x5558AC, "rest spin x"},
         {0x507C92, 0xD8, kFmul, 0x5558AC, "rest spin y"},
         {0x507CA4, 0xD8, kFmul, 0x5558AC, "rest spin z"}}},
    {5, {{0x507CB3, 0xD8, kFmul, 0x55D0C0, "rest vel x"},
         {0x507CBF, 0xD8, kFmul, 0x55D0C0, "rest vel y"},
         {0x507CCB, 0xD8, kFmul, 0x55D0C0, "rest vel z"},
         {0x507CD5, 0xD8, kFmul, 0x55D0C0, "settle spin x"},
         {0x507CE7, 0xD8, kFmul, 0x55D0C0, "settle spin y"},
         {0x507CF9, 0xD8, kFmul, 0x55D0C0, "settle spin z"}}},
    // Vehicle update outside the full simulation (0x52ffb0).
    {6, {{0x53098D, 0xD8, kFmul, 0x55DFA4, "vehicle spin x"},
         {0x5309A3, 0xD8, kFmul, 0x55DFA4, "vehicle spin y"},
         {0x5309B5, 0xD8, kFmul, 0x55DFA4, "vehicle spin z"}}},
    // Bike camera zoom (0x466b30).
    {7, {{0x4671B2, 0xD8, kFmul, 0x555904, "camera zoom"}}},
};

bool g_fix_frames = false;
float g_ref_rate = 30.0f;

void UpdateFixValues(float dt) {
  if (!g_fix_frames) return;
  if (dt > 0.2f) dt = 0.2f;  // the game clamps its step the same way
  const float frames = dt * g_ref_rate;
  for (FixValue& v : g_values) {
    v.value = v.kind == Kind::kStep ? v.k * frames : std::pow(v.k, frames);
  }
}

void PatchFixSites() {
  int done = 0, total = 0;
  for (const SiteGroup& g : kGroups) {
    for (const FixSite& s : g.sites) {
      if (!s.addr) continue;
      ++total;
      uint8_t expect[6] = {s.op0, s.op1};
      uint32_t c = static_cast<uint32_t>(s.constant);
      std::memcpy(expect + 2, &c, 4);
      if (!patch::Matches(s.addr, expect, sizeof(expect))) {
        patch::Log("frame-rate fix: %s at %08x differs, skipped", s.what,
                   static_cast<unsigned>(s.addr));
        continue;
      }
      uint32_t ours = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_values[g.value].value));
      patch::Write(s.addr + 2, &ours, 4);
      ++done;
    }
  }
  patch::Log("frame-rate fix: %d of %d spots now scale with frame time (RefRate=%g)", done, total,
             g_ref_rate);
}

// ---------------------------------------------------------------- AI decision rate
//
// KrustyBike's per-frame control hook (0x493470) calls the AI planner
// (BikeAI, 0x415170) every frame for computer riders. It picks a target point
// and speed from the track and nearby riders; at high frame rates a choice
// that flips between two answers flips much more often, which shows as
// riders twitching left and right near each other. With AiRate set, we run
// the planner at that rate per rider, handing it the time since its last
// run, and the bike simulation keeps following the last plan in between.

using AiPlanFn = int(__thiscall*)(void* bike, float dt);
const AiPlanFn kAiPlan = reinterpret_cast<AiPlanFn>(0x415170);

struct AiState {
  float pending = 0.0f;
  int last = 1;
  LONGLONG seen = 0;  // QPC of the last call
};
std::map<void*, AiState> g_ai;

int __stdcall AiPlan(void* bike, float dt) {
  const int rate = GetConfig().ai_rate;
  if (rate <= 0) return kAiPlan(bike, dt);
  AiState& st = g_ai[bike];
  const LONGLONG now = Now();
  // A rider we haven't seen for a moment is a new race (bikes of a new race
  // can reuse an old bike's memory): always plan on its first call, like the
  // original game does, instead of reusing the old race's answer.
  const bool fresh = st.seen == 0 || now - st.seen > g_freq.QuadPart / 2;
  st.seen = now;
  if (fresh) st.pending = 0.0f;
  st.pending += dt;
  if (!fresh && st.pending + 1e-4f < 1.0f / rate && st.pending < 0.2f) return st.last;
  float step = st.pending;
  st.pending = 0.0f;
  st.last = kAiPlan(bike, step);
  return st.last;
}

void PatchAiRate() {
  const uint8_t call_plan[] = {0xE8, 0xEA, 0x1C, 0xF8, 0xFF};  // 00493481 call 415170
  constexpr uintptr_t kAt = 0x493481;
  if (!patch::Matches(kAt, call_plan, sizeof(call_plan))) {
    patch::Log("AI rate: call site differs, not applied");
    return;
  }
  // Stub: thiscall (ecx = bike, [esp+4] = dt) -> AiPlan(bike, dt).
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  size_t n = 0;
  const uint8_t head[] = {0xFF, 0x74, 0x24, 0x04, 0x51, 0xE8};  // push [esp+4]; push ecx; call
  std::memcpy(t, head, sizeof(head));
  n = sizeof(head);
  int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&AiPlan) -
                                     (reinterpret_cast<uintptr_t>(t) + n + 4));
  std::memcpy(t + n, &rel, 4);
  n += 4;
  const uint8_t tail[] = {0xC2, 0x04, 0x00};  // ret 4
  std::memcpy(t + n, tail, sizeof(tail));
  n += sizeof(tail);
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Call(kAt, t, sizeof(call_plan));
  patch::Log("AI rate: planner runs at %d Hz per rider", GetConfig().ai_rate);
}

// ---------------------------------------------------------------- AI steering speed
//
// Computer riders do not use the controls. Inside the bike physics (0x40faa0)
// their front wheel is pointed straight at the planner's target point every
// step, clamped to 45 degrees, with no limit on how fast it swings, so it can
// go from full left to full right in one frame. Two copies of that code end
// at 0x41021c and 0x413571 with the target direction (x, y, z) at
// [esp+30h] and the bike's heading at [esp+14h]; the rider's path struct
// (unique per rider) is at [esp+998h]. We limit how fast the wheel angle
// may change, AiSteerRate degrees per second.

struct AiWheel {
  float angle = 0.0f;  // wheel angle we let through last time, radians
  float base = 0.0f;   // angle at the start of this tick
  unsigned tick = 0;
};
std::map<uintptr_t, AiWheel> g_ai_wheel;
unsigned g_tick = 0;     // counts game ticks
float g_tick_dt = 0.0f;  // the game's (clamped) step for this tick

void __stdcall LimitAiWheel(uintptr_t esp) {
  const int rate = GetConfig().ai_steer_rate;
  if (rate <= 0 || g_tick_dt <= 0.0f) return;
  auto* target = reinterpret_cast<float*>(esp + 0x30);
  const auto* heading = reinterpret_cast<const float*>(esp + 0x14);
  const uintptr_t rider = *reinterpret_cast<uintptr_t*>(esp + 0x998);
  float hx = heading[0], hz = heading[2];
  float hl = std::sqrt(hx * hx + hz * hz);
  float tl = std::sqrt(target[0] * target[0] + target[2] * target[2]);
  if (hl < 1e-6f || tl < 1e-6f) return;
  hx /= hl;
  hz /= hl;
  const float tx = target[0] / tl, tz = target[2] / tl;
  const float want = std::atan2(hx * tz - hz * tx, hx * tx + hz * tz);
  AiWheel& w = g_ai_wheel[rider];
  if (w.tick != g_tick) {
    if (g_tick - w.tick > 2) w.angle = want;  // new rider or after a pause
    w.base = w.angle;
    w.tick = g_tick;
  }
  const float max_step = rate * 3.14159265f / 180.0f * g_tick_dt;
  float d = want - w.base;
  if (d > max_step) d = max_step;
  if (d < -max_step) d = -max_step;
  w.angle = w.base + d;
  const float c = std::cos(w.angle), sn = std::sin(w.angle);
  target[0] = hx * c - hz * sn;
  target[1] = 0.0f;
  target[2] = hz * c + hx * sn;
}

void HookAiWheel(uintptr_t at, const uint8_t (&original)[8]) {
  if (!patch::Matches(at, original, sizeof(original))) {
    patch::Log("AI steering: code at %08x differs, not applied", static_cast<unsigned>(at));
    return;
  }
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  size_t n = 0;
  const uint8_t head[] = {0x60, 0x8D, 0x44, 0x24, 0x20, 0x50, 0xE8};  // pushad; lea eax,[esp+20h]; push eax; call
  std::memcpy(t, head, sizeof(head));
  n = sizeof(head);
  int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LimitAiWheel) -
                                     (reinterpret_cast<uintptr_t>(t) + n + 4));
  std::memcpy(t + n, &rel, 4);
  n += 4;
  t[n++] = 0x61;  // popad
  std::memcpy(t + n, original, sizeof(original));  // the replaced instructions
  n += sizeof(original);
  t[n++] = 0x68;  // push resume
  uint32_t resume = static_cast<uint32_t>(at + sizeof(original));
  std::memcpy(t + n, &resume, 4);
  n += 4;
  t[n++] = 0xC3;  // ret
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(at, reinterpret_cast<uintptr_t>(t), sizeof(original));
}

void PatchAiSteering() {
  // fld dword [esp+30h] / mov ecx,[esp+38h]
  const uint8_t first[8] = {0xD9, 0x44, 0x24, 0x30, 0x8B, 0x4C, 0x24, 0x38};
  // fld dword [esp+30h] / mov eax,[esp+38h]
  const uint8_t second[8] = {0xD9, 0x44, 0x24, 0x30, 0x8B, 0x44, 0x24, 0x38};
  HookAiWheel(0x41021C, first);
  HookAiWheel(0x413571, second);
  patch::Log("AI steering: front wheel turns at most %d degrees/s", GetConfig().ai_steer_rate);
}

// ---------------------------------------------------------------- frame time

using MsTimerFn = DWORD(__cdecl*)();
const MsTimerFn kGameMs = reinterpret_cast<MsTimerFn>(0x4C41D0);

// Replaces Game::Tick's frame-time code (0x46960b-0x469651). The game keeps
// its millisecond stamps at +0x2d8 (this frame) and +0x2dc (last frame,
// zero after a reset); we keep them up to date for anything else reading
// them and return the step in seconds on the FPU stack, like the original.
float __stdcall GameFrameTime(uintptr_t game) {
  auto& now_ms = *reinterpret_cast<DWORD*>(game + 0x2d8);
  auto& last_ms = *reinterpret_cast<DWORD*>(game + 0x2dc);
  LONGLONG now = Now();
  float dt = 0.0f;
  if (last_ms == 0 || g_last_tick == 0) {
    now_ms = last_ms = kGameMs();
  } else {
    now_ms = kGameMs();
    dt = static_cast<float>(static_cast<double>(now - g_last_tick) / g_freq.QuadPart);
  }
  g_last_tick = now;
  if (!GetConfig().precise_timer) {
    dt = (now_ms - last_ms) * 0.001f;  // the game's own whole-millisecond step
  }
  ++g_tick;
  g_tick_dt = dt > 0.2f ? 0.2f : dt;
  UpdateFixValues(dt);
  return dt;
}

void PatchFrameTime() {
  // 0046960b  mov eax,[edi+2DCh] / test / jne / mov ecx,edi / call 469760 /
  //           jmp / call 4c41d0 / mov [edi+2D8h],eax / ... /
  // 0046964f  lea ecx,[eax+eax*8] / fmul dword [555888h] (ms -> s)
  const uint8_t original[] = {
      0x8B, 0x87, 0xDC, 0x02, 0x00, 0x00,        // mov eax,[edi+2DCh]
      0x85, 0xC0,                                // test eax,eax
      0x75, 0x09,                                // jne 46961e
      0x8B, 0xCF,                                // mov ecx,edi
      0xE8, 0x44, 0x01, 0x00, 0x00,              // call 469760
      0xEB, 0x0B,                                // jmp 469629
      0xE8, 0xAD, 0xAB, 0x05, 0x00,              // call 4c41d0
      0x89, 0x87, 0xD8, 0x02, 0x00, 0x00,        // mov [edi+2D8h],eax
      0x8B, 0x87, 0xD8, 0x02, 0x00, 0x00,        // mov eax,[edi+2D8h]
      0x8B, 0x8F, 0xDC, 0x02, 0x00, 0x00,        // mov ecx,[edi+2DCh]
      0x2B, 0xC1,                                // sub eax,ecx
      0xC7, 0x45, 0xF8, 0x00, 0x00, 0x00, 0x00,  // mov dword [ebp-8],0
      0x89, 0x45, 0xF4,                          // mov [ebp-0Ch],eax
      0xA1, 0xF8, 0x15, 0x66, 0x00,              // mov eax,[6615F8h]
      0xDF, 0x6D, 0xF4,                          // fild qword [ebp-0Ch]
      0x8B, 0x15, 0xF0, 0x24, 0x57, 0x00,        // mov edx,[5724F0h]
      0x8D, 0x0C, 0xC0,                          // lea ecx,[eax+eax*8]
      0xD8, 0x0D, 0x88, 0x58, 0x55, 0x00,        // fmul dword [555888h]
  };
  constexpr uintptr_t kAt = 0x46960B;
  if (!patch::Matches(kAt, original, sizeof(original))) {
    patch::Log("precise frame time: Game::Tick code differs, not applied");
    return;
  }
  uint8_t code[sizeof(original)];
  std::memset(code, 0x90, sizeof(code));
  size_t n = 0;
  code[n++] = 0x57;  // push edi
  code[n++] = 0xE8;  // call GameFrameTime (rel32 filled below)
  int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&GameFrameTime) - (kAt + n + 4));
  std::memcpy(code + n, &rel, 4);
  n += 4;
  const uint8_t tail[] = {
      0xA1, 0xF8, 0x15, 0x66, 0x00,        // mov eax,[6615F8h]
      0x8B, 0x15, 0xF0, 0x24, 0x57, 0x00,  // mov edx,[5724F0h]
      0x8D, 0x0C, 0xC0,                    // lea ecx,[eax+eax*8]
  };
  std::memcpy(code + n, tail, sizeof(tail));
  patch::Write(kAt, code, sizeof(code));
  patch::Log("precise frame time: applied");
}

// ---------------------------------------------------------------- fixed-rate simulation
//
// Game::Tick (0x4694b0, also reached from TrackGame's tick at 0x526cb0):
//   frame time -> ControlInterface (+0x14) update(dt)
//   unless paused (+0x2d5 bit 0):
//     manager (+0x8) update(dt), world state (+0x2f4) vfunc 0x28 (dt),
//     profiler 0x4894a0, Draw (vfunc 0x20), Present (vfunc 0x24),
//     state post-frame 0x46abb0, 0x4eda50 on +0x10
// The physics and AI were tuned at ~30 fps and do not behave the same with
// smaller steps. With SimRate set, while a race is running we replace the
// tick: the update calls run in fixed 1/SimRate steps, as many as real time
// calls for, and draw/present runs once per displayed frame. Outside races
// (menus, cursor) the tick runs once per frame with the real time, as before.

using ThisFloatFn = int(__thiscall*)(void*, float);
using ThisFn = int(__thiscall*)(void*);
const ThisFloatFn kControlUpdate = reinterpret_cast<ThisFloatFn>(0x43DEA0);
const ThisFloatFn kManagerUpdate = reinterpret_cast<ThisFloatFn>(0x4B14D0);
const ThisFn kProfilerFrame = reinterpret_cast<ThisFn>(0x4894A0);
const ThisFn kStatePostFrame = reinterpret_cast<ThisFn>(0x46ABB0);
const ThisFn kPostFrame10 = reinterpret_cast<ThisFn>(0x4EDA50);

LONGLONG g_bike_seen = 0;  // QPC when a bike was last simulated (race running)
double g_sim_acc = 0.0;
bool g_sim_fixed = false;

template <typename Fn>
Fn VFunc(void* obj, size_t offset) {
  return *reinterpret_cast<Fn*>(*reinterpret_cast<uintptr_t*>(obj) + offset);
}

void SimUpdate(uintptr_t g, float step, float raw_dt) {
  *reinterpret_cast<float*>(g + 0x2f0) = raw_dt;  // the game's global frame time
  ++g_tick;
  g_tick_dt = step;
  UpdateFixValues(step);
  if (void* ctl = *reinterpret_cast<void**>(g + 0x14)) kControlUpdate(ctl, step);
  if (*reinterpret_cast<uint8_t*>(g + 0x2d5) & 1) return;  // paused
  if (void* mgr = *reinterpret_cast<void**>(g + 0x8)) kManagerUpdate(mgr, step);
  void* state = *reinterpret_cast<void**>(g + 0x2f4);
  VFunc<ThisFloatFn>(state, 0x28)(state, step);
}

int __stdcall FixedTick(uintptr_t g) {
  auto& now_ms = *reinterpret_cast<DWORD*>(g + 0x2d8);
  auto& last_ms = *reinterpret_cast<DWORD*>(g + 0x2dc);
  const LONGLONG now = Now();
  double dt = 0.0;
  if (last_ms == 0 || g_last_tick == 0) {
    now_ms = last_ms = kGameMs();
    g_sim_acc = 0.0;
  } else {
    now_ms = kGameMs();
    dt = static_cast<double>(now - g_last_tick) / g_freq.QuadPart;
  }
  g_last_tick = now;

  const int rate = GetConfig().sim_rate;
  const bool racing = g_bike_seen && now - g_bike_seen < g_freq.QuadPart / 2;
  if (racing != g_sim_fixed) {
    g_sim_fixed = racing;
    g_sim_acc = 0.0;
    patch::Log("fixed-rate simulation %s", racing ? "on (race)" : "off");
  }
  if (g_sim_fixed) {
    const double step = 1.0 / rate;
    g_sim_acc += dt;
    if (g_sim_acc > 0.25) g_sim_acc = 0.25;  // after a stall, don't try to catch up
    while (g_sim_acc >= step) {
      SimUpdate(g, static_cast<float>(step), static_cast<float>(step));
      g_sim_acc -= step;
    }
  } else {
    const float raw = static_cast<float>(dt);
    SimUpdate(g, raw > 0.2f ? 0.2f : raw, raw);
  }

  if (!(*reinterpret_cast<uint8_t*>(g + 0x2d5) & 1)) {
    kProfilerFrame(*reinterpret_cast<void**>(0x5724F0));
    VFunc<ThisFn>(reinterpret_cast<void*>(g), 0x20)(reinterpret_cast<void*>(g));
    VFunc<ThisFn>(reinterpret_cast<void*>(g), 0x24)(reinterpret_cast<void*>(g));
    kStatePostFrame(*reinterpret_cast<void**>(g + 0x2f4));
    kPostFrame10(*reinterpret_cast<void**>(g + 0x10));
  }
  last_ms = now_ms;
  if (*reinterpret_cast<int*>(g + 0x1c8)) {
    *reinterpret_cast<int*>(g + 0x1c4) = 1;
    *reinterpret_cast<int*>(g + 0x1c8) = 0;
  }
  return 1;
}

void __stdcall NoteBikeSim() { g_bike_seen = Now(); }

void PatchFixedTick() {
  const uint8_t entry[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C};  // 004694b0 push ebp / mov ebp,esp / sub esp,0Ch
  const uint8_t call_sim[] = {0xE8, 0x24, 0xC4, 0xF7, 0xFF};     // 00497bd7 call 414000
  if (!patch::Matches(0x4694B0, entry, sizeof(entry)) ||
      !patch::Matches(0x497BD7, call_sim, sizeof(call_sim))) {
    patch::Log("fixed-rate simulation: code differs, not applied");
    return;
  }
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  auto rel = [](const uint8_t* from_end, const void* to) {
    return static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - reinterpret_cast<uintptr_t>(from_end));
  };
  // Tick stub at t: push ecx; call FixedTick; ret
  size_t n = 0;
  t[n++] = 0x51;
  t[n++] = 0xE8;
  int32_t r = rel(t + n + 4, reinterpret_cast<const void*>(&FixedTick));
  std::memcpy(t + n, &r, 4);
  n += 4;
  t[n++] = 0xC3;
  // Bike stub at t+16: pushad; call NoteBikeSim; popad; jmp 414000
  uint8_t* b = t + 16;
  size_t m = 0;
  b[m++] = 0x60;
  b[m++] = 0xE8;
  r = rel(b + m + 4, reinterpret_cast<const void*>(&NoteBikeSim));
  std::memcpy(b + m, &r, 4);
  m += 4;
  b[m++] = 0x61;
  b[m++] = 0xE9;
  r = rel(b + m + 4, reinterpret_cast<const void*>(0x414000));
  std::memcpy(b + m, &r, 4);
  m += 4;
  FlushInstructionCache(GetCurrentProcess(), t, 64);
  patch::Jump(0x4694B0, reinterpret_cast<uintptr_t>(t), sizeof(entry));
  patch::Call(0x497BD7, b, sizeof(call_sim));
  patch::Log("fixed-rate simulation: %d steps per second in races", GetConfig().sim_rate);
}

void SleepUntil(LONGLONG target) {
  for (;;) {
    LONGLONG left = target - Now();
    if (left <= 0) return;
    // Sleep in a timer while more than ~1.5 ms remain, then spin.
    LONGLONG left_100ns = left * 10000000 / g_freq.QuadPart;
    if (left_100ns > 15000 && g_timer) {
      LARGE_INTEGER due;
      due.QuadPart = -(left_100ns - 10000);
      if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(g_timer, INFINITE);
        continue;
      }
    }
    if (left_100ns > 15000) {
      Sleep(1);
    } else {
      YieldProcessor();
    }
  }
}

}  // namespace

void ApplyFramePacing() {
  QueryPerformanceFrequency(&g_freq);
  const Config& c = GetConfig();
  if (c.frame_cap > 0) {
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+), else a normal one.
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  g_fix_frames = c.frame_fix;
  g_ref_rate = static_cast<float>(c.ref_rate);
  if (c.frame_fix) PatchFixSites();
  // With a fixed simulation rate the planner already runs at most SimRate
  // times a second, so the separate AI throttle is only needed below that.
  if (c.ai_rate > 0 && (c.sim_rate <= 0 || c.ai_rate < c.sim_rate)) PatchAiRate();
  if (c.ai_steer_rate > 0) PatchAiSteering();
  if (c.sim_rate > 0) {
    PatchFixedTick();  // replaces the whole tick, frame time included
    if (c.sim_smooth) InstallTextSmoothing();
  } else if (c.precise_timer || c.frame_fix || c.ai_steer_rate > 0) {
    PatchFrameTime();
  }
  patch::Log("frame pacing: VSync=%d FrameCap=%d PreciseTimer=%d FrameFix=%d RefRate=%d AiRate=%d "
             "AiSteerRate=%d SimRate=%d",
             c.vsync, c.frame_cap, c.precise_timer, c.frame_fix, c.ref_rate, c.ai_rate,
             c.ai_steer_rate, c.sim_rate);
}

bool SimDrawPoint(unsigned* tick, float* alpha) {
  if (!g_sim_fixed || !GetConfig().sim_smooth) return false;
  const double step = 1.0 / GetConfig().sim_rate;
  float a = static_cast<float>(g_sim_acc / step);
  if (a < 0.0f) a = 0.0f;
  if (a > 1.0f) a = 1.0f;
  *tick = g_tick;
  *alpha = a;
  return true;
}

void WaitForFrameSlot() {
  const int cap = GetConfig().frame_cap;
  if (cap <= 0 || !g_freq.QuadPart) return;
  const LONGLONG period = g_freq.QuadPart / cap;
  LONGLONG now = Now();
  if (g_next_frame == 0 || now - g_next_frame > period) {
    g_next_frame = now + period;  // first frame, or fell behind: start over
    return;
  }
  SleepUntil(g_next_frame);
  g_next_frame += period;
}
