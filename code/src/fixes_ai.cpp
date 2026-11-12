// Computer rider difficulty tuning (AiAccel*, AiTopSpeed*, AiLog).
//
// The game's Easy / Normal / Hard setting ([0x5743ac]+0x60c, 1..3) changes a
// computer rider's acceleration and nothing about its top speed (see
// mcm2-docs/ai-difficulty.md):
//   - KrustyBike constructor 0x490c30 stores acceleration multipliers 1.1 /
//     1.3 / 1.5 at bike+0x7c4/+0x7c8/+0x7cc (mov imm32 at 0x490c60..0x490c7d);
//     the planner copies the current one to bike+0x11a8.
//   - The AI controller 0x40faa0 caps acceleration at multiplier x
//     table[speed], one 200-float table per engine class at 0x56d05c. The
//     table pointer is worked out at 0x414790 (class at bike+0x79c).
//
// AiAccel* replaces the three multipliers. AiTopSpeed* stretches the speed
// axis of the table per difficulty (90 = the curve reaches 0 at 90 % of the
// speed), which is the only way the AI's top speed can be changed: the code
// at 0x414790 is replaced by a call that returns our table for the current
// difficulty. Only computer riders use these tables (0x414000 runs only for
// them); the player's bike is untouched.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kGame = 0x5743ac;
constexpr uintptr_t kTables = 0x56d05c;  // 3 x 200 floats
constexpr int kEntries = 200;

float g_tab[3][3][kEntries];  // [difficulty-1][engine class][speed]
int g_last_diff = 0;

// AiLog: highest speed per rider over 5 s windows.
struct Watch {
  void* bike = nullptr;
  float max_speed = 0.0f;
  int cls = 0;
  float mult = 0.0f;
};
Watch g_watch[16];

// AiLog: per-rider jump and teleport tracking (computer riders and the player).
struct Track {
  void* bike = nullptr;
  bool air = false;
  float air_time = 0.0f;
  float takeoff_speed = 0.0f;
  float pos[3] = {};
  bool has_pos = false;
  DWORD seen = 0;
};
Track g_track[16];
DWORD g_watch_start = 0;
int g_watch_diff = 0;

int Difficulty() {
  const uintptr_t g = *reinterpret_cast<uintptr_t*>(kGame);
  if (!g) return 2;
  int d = *reinterpret_cast<int*>(g + 0x60c);
  if (d < 1 || d > 3) d = 2;
  return d;
}

int EventType() {
  const uintptr_t g = *reinterpret_cast<uintptr_t*>(kGame);
  return g ? *reinterpret_cast<int*>(g + 0x2d74) : -1;
}

void LogWatch(DWORD now) {
  static const char* kEvent[] = {"quarry", "baja", "nationals", "supercross", "tag", "enduro"};
  const int ev = EventType();
  char line[1024];
  int n = std::snprintf(line, sizeof(line), "ai: %s difficulty=%d, max speed over %.1f s (ft/s):",
                        ev >= 0 && ev < 6 ? kEvent[ev] : "?", g_watch_diff,
                        (now - g_watch_start) / 1000.0f);
  for (Watch& w : g_watch) {
    if (!w.bike || n >= static_cast<int>(sizeof(line)) - 32) continue;
    n += std::snprintf(line + n, sizeof(line) - n, " %.0f(c%d x%.2f)", w.max_speed, w.cls,
                       w.mult);
    w = Watch();
  }
  patch::Log("%s", line);
  g_watch_start = now;
}

float StepTime() {
  const uintptr_t g = *reinterpret_cast<uintptr_t*>(kGame);
  float dt = g ? *reinterpret_cast<float*>(g + 0x2f0) : 0.0f;
  if (!(dt > 0.0f) || dt > 0.2f) dt = 1.0f / 30.0f;
  return dt;
}

Track* FindTrack(void* bike) {
  const DWORD now = GetTickCount();
  for (Track& t : g_track) {
    if (t.bike == bike && now - t.seen < 1000) {
      t.seen = now;
      return &t;
    }
  }
  for (Track& t : g_track) {
    if (!t.bike || now - t.seen >= 1000) {  // free, or a bike of an earlier race
      t = Track();
      t.seen = now;
      t = Track();
      t.bike = bike;
      return &t;
    }
  }
  return nullptr;
}

// Logs takeoff/landing speeds and position jumps (respawns) for one bike.
void TrackBike(void* bike, const char* who, float dt) {
  Track* t = FindTrack(bike);
  if (!t) return;
  const uint8_t* b = static_cast<uint8_t*>(bike);
  const float* pos = reinterpret_cast<const float*>(b + 0xc);
  const float speed = *reinterpret_cast<const float*>(b + 0xbc);
  const bool ground = b[0x108] != 0;
  const bool crashed = *reinterpret_cast<const int*>(b + 0x444) != 0;
  if (t->has_pos) {
    const float dx = pos[0] - t->pos[0], dy = pos[1] - t->pos[1], dz = pos[2] - t->pos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > 30.0f * 30.0f) {
      patch::Log("ai: %s %p moved %.0f ft in one step (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f)", who,
                 bike, std::sqrt(d2), t->pos[0], t->pos[1], t->pos[2], pos[0], pos[1], pos[2]);
    }
  }
  std::memcpy(t->pos, pos, sizeof(t->pos));
  t->has_pos = true;
  if (crashed) {
    t->air = false;
    return;
  }
  if (!ground && !t->air) {
    t->air = true;
    t->air_time = 0.0f;
    t->takeoff_speed = speed;
  } else if (!ground) {
    t->air_time += dt;
  } else if (t->air) {
    t->air = false;
    if (t->air_time >= 0.3f) {
      patch::Log("ai: %s %p jump %.2f s, speed %.1f -> %.1f ft/s (%+.0f %%)", who, bike,
                 t->air_time, t->takeoff_speed, speed,
                 t->takeoff_speed > 1.0f ? (speed / t->takeoff_speed - 1.0f) * 100.0f : 0.0f);
    }
  }
}

// AiAirDrag: computer riders lose this many percent of their horizontal speed
// per second while airborne (+0x108 = on the ground; velocity at +0x64).
void AirDrag(void* bike, float dt) {
  const int pct = GetConfig().ai_air_drag;
  if (pct <= 0) return;
  uint8_t* b = static_cast<uint8_t*>(bike);
  if (b[0x108] != 0 || *reinterpret_cast<int*>(b + 0x444) != 0) return;
  float k = 1.0f - pct / 100.0f * dt;
  if (k < 0.0f) k = 0.0f;
  float* vel = reinterpret_cast<float*>(b + 0x64);
  vel[0] *= k;
  vel[2] *= k;
}

// Called from the stub at 0x414790 for every computer rider step.
const float* __stdcall AiTable(void* bike, int cls) {
  if (cls < 0 || cls > 2) cls = 0;
  // 0x414000 also runs for the player's bike in the game's demo/autopilot
  // mode; leave that bike exactly as the original.
  if (bike && static_cast<uint8_t*>(bike)[0x734] == 0) {
    return reinterpret_cast<const float*>(kTables) + cls * kEntries;
  }
  const int d = Difficulty();
  if (d != g_last_diff) {
    const Config& c = GetConfig();
    patch::Log("ai: difficulty %d in use (event %d): acceleration x%.2f, top speed %d %%", d,
               EventType(), c.ai_accel[d - 1] / 100.0f, c.ai_top_speed[d - 1]);
    g_last_diff = d;
  }
  const float dt = StepTime();
  if (bike) AirDrag(bike, dt);
  if (GetConfig().ai_log && bike) {
    // The player's bike: BikeRace (bike+0x740) +0x38.
    const uintptr_t race = *reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(bike) + 0x740);
    void* player = race ? *reinterpret_cast<void**>(race + 0x38) : nullptr;
    static DWORD s_player_at = 0;  // track the player at most once per step
    const DWORD tick = GetTickCount();
    if (player && player != bike && tick - s_player_at >= 25) {
      const float pdt = s_player_at && tick - s_player_at < 200 ? (tick - s_player_at) / 1000.0f : dt;
      s_player_at = tick;
      TrackBike(player, "player", pdt);
    }
    TrackBike(bike, "ai", dt);
    const DWORD now = GetTickCount();
    if (!g_watch_start) g_watch_start = now;
    if (now - g_watch_start >= 5000 || d != g_watch_diff) {
      if (g_watch_diff) LogWatch(now);
      g_watch_diff = d;
      g_watch_start = now;
    }
    const float speed = *reinterpret_cast<float*>(static_cast<uint8_t*>(bike) + 0xbc);
    for (Watch& w : g_watch) {
      if (w.bike == bike || !w.bike) {
        w.bike = bike;
        w.cls = cls;
        w.mult = *reinterpret_cast<float*>(static_cast<uint8_t*>(bike) + 0x11a8);
        if (speed > w.max_speed) w.max_speed = speed;
        break;
      }
    }
  }
  return g_tab[d - 1][cls];
}

void BuildTables() {
  const Config& c = GetConfig();
  const float* orig = reinterpret_cast<const float*>(kTables);
  for (int d = 0; d < 3; ++d) {
    const float s = c.ai_top_speed[d] / 100.0f;
    for (int cls = 0; cls < 3; ++cls) {
      for (int i = 0; i < kEntries; ++i) {
        const int j = static_cast<int>(i / s);  // speed i on the new curve = speed i/s on the old
        g_tab[d][cls][i] = j < kEntries ? orig[cls * kEntries + j] : 0.0f;
      }
    }
  }
}

bool PatchMultipliers() {
  // 00490c60 mov [ebp+7c4],3f8ccccd / mov [ebp+7c8],3fa66666 / mov [ebp+7cc],3fc00000
  const uint8_t orig[] = {0xC7, 0x85, 0xC4, 0x07, 0x00, 0x00, 0xCD, 0xCC, 0x8C, 0x3F,
                          0xC7, 0x85, 0xC8, 0x07, 0x00, 0x00, 0x66, 0x66, 0xA6, 0x3F,
                          0xC7, 0x85, 0xCC, 0x07, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x3F};
  uint8_t repl[sizeof(orig)];
  std::memcpy(repl, orig, sizeof(orig));
  for (int d = 0; d < 3; ++d) {
    const float v = GetConfig().ai_accel[d] / 100.0f;
    std::memcpy(repl + d * 10 + 6, &v, 4);
  }
  return patch::Replace("ai: acceleration multipliers", 0x490c60, orig, sizeof(orig), repl,
                        sizeof(repl));
}

bool PatchTablePointer() {
  // 00414790 lea ecx,[ecx+ecx*4]; lea ecx,[ecx+ecx*4]; shl ecx,5; add ecx,56d05c
  const uint8_t orig[] = {0x8D, 0x0C, 0x89, 0x8D, 0x0C, 0x89, 0xC1, 0xE1,
                          0x05, 0x81, 0xC1, 0x5C, 0xD0, 0x56, 0x00};
  constexpr uintptr_t kAt = 0x414790;
  if (!patch::Matches(kAt, orig, sizeof(orig))) {
    patch::Log("ai: table pointer code differs, AiTopSpeed not applied");
    return false;
  }
  BuildTables();
  // Stub: keep eax/edx, ecx = AiTable(esi = bike, ecx = class).
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return false;
  size_t n = 0;
  const uint8_t head[] = {0x50, 0x52, 0x51, 0x56, 0xE8};  // push eax; push edx; push ecx; push esi; call
  std::memcpy(t, head, sizeof(head));
  n = sizeof(head);
  const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&AiTable) -
                                           (reinterpret_cast<uintptr_t>(t) + n + 4));
  std::memcpy(t + n, &rel, 4);
  n += 4;
  const uint8_t tail[] = {0x89, 0xC1, 0x5A, 0x58, 0xC3};  // mov ecx,eax; pop edx; pop eax; ret
  std::memcpy(t + n, tail, sizeof(tail));
  n += sizeof(tail);
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Call(kAt, t, sizeof(orig));
  return true;
}

}  // namespace

void ApplyAiFixes() {
  const Config& c = GetConfig();
  const bool accel_changed = c.ai_accel[0] != 110 || c.ai_accel[1] != 130 || c.ai_accel[2] != 150;
  const bool top_changed =
      c.ai_top_speed[0] != 100 || c.ai_top_speed[1] != 100 || c.ai_top_speed[2] != 100;
  if (accel_changed) PatchMultipliers();
  if (top_changed || c.ai_log || c.ai_air_drag > 0) PatchTablePointer();
  patch::Log("ai: acceleration %d/%d/%d %%, top speed %d/%d/%d %% (Easy/Normal/Hard), "
             "air drag %d %%/s%s",
             c.ai_accel[0], c.ai_accel[1], c.ai_accel[2], c.ai_top_speed[0],
             c.ai_top_speed[1], c.ai_top_speed[2], c.ai_air_drag, c.ai_log ? ", AiLog on" : "");
}
