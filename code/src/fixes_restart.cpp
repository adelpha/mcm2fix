// "Restart Race" from the pause menu (ExitDlg, ButMiddle -> BikeRace
// restart at 0x421590). The game puts every bike back on the start grid:
// it recomputes each grid slot with 0x422030 and stores the position at
// bike+0x10c and the heading at bike+0x118, then sets bike+0x109 so the
// bike applies it on its next update.
//
// Test build: RestartLog=1 logs what the grid code computes at race load
// and at restart, and what the bikes look like on the frames after, so we
// can see where the wrong positions and headings come from.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "config.h"
#include "patch.h"

void SoundTraceStart(const char* tag, DWORD ms);

namespace {

constexpr uintptr_t kGame = 0x5743AC;           // global game object pointer
constexpr uintptr_t kRestart = 0x421590;        // BikeRace restart (this, a, b, c)
constexpr uintptr_t kGridSlot = 0x422030;       // BikeRace grid slot (this, pos*, dir*, event, slot)
constexpr uintptr_t kNextBike = 0x421420;       // BikeRace bike iterator (this, int* idx)
constexpr uintptr_t kBikeRaceVtable = 0x555DDC;
constexpr int kUpdateSlot = 10;

const uint8_t kRestartEntry[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};  // mov eax, fs:[0]
const uint8_t kGridSlotEntry[] = {0x83, 0xEC, 0x6C, 0x53, 0x55};      // sub esp,6ch; push ebx; push ebp

using RestartFn = void(__fastcall*)(uintptr_t self, void* edx, int a, int b, int c);
using GridSlotFn = void(__fastcall*)(uintptr_t self, void* edx, float* pos, float* dir,
                                     uintptr_t event, int slot);
using NextBikeFn = uintptr_t(__fastcall*)(uintptr_t self, void* edx, int* idx);
using UpdateFn = void(__fastcall*)(uintptr_t self, void* edx, int arg);

RestartFn g_restart = nullptr;
GridSlotFn g_grid_slot = nullptr;
UpdateFn g_update = nullptr;

uintptr_t g_race = 0;    // BikeRace being watched
int g_frame = -1;        // frames since load/restart (-1 = not watching)
const char* g_phase = "load";
int g_grid_calls = 0;
unsigned g_restarts = 0;  // Restart Race count

template <typename T>
T Rd(uintptr_t a) {
  T v{};
  if (a && !IsBadReadPtr(reinterpret_cast<void*>(a), sizeof(T))) std::memcpy(&v, reinterpret_cast<void*>(a), sizeof(T));
  return v;
}

uintptr_t Game() { return Rd<uintptr_t>(kGame); }

// ---- FINAL LAP after a restart ----
// The race HUD shows "FINAL LAP" once: its update (slot 10; 0x405c10 for
// vtable 0x555750, 0x50a750 for the other HUD) starts the drop text at
// +0xa8 when the player starts the last lap and then sets a "shown" flag
// (+0xa4 or +0xac) that only the constructor clears. Restart Race keeps
// the HUD, so after a restart the text never shows again. Clear the flag
// (and stop a drop still in progress) on the first HUD update after each
// restart.
struct HudHook {
  uintptr_t slot, fn, flag;
};
const HudHook kHudHooks[] = {{0x555778, 0x405C10, 0xA4}, {0x55A610, 0x50A750, 0xAC}, {0x55D560, 0x50A750, 0xAC}};
struct HudSeen {
  uintptr_t hud;
  unsigned restarts;
};
HudSeen g_huds[8] = {};

void HudRestartCheck(uintptr_t self, uintptr_t flag) {
  HudSeen* free_slot = nullptr;
  for (HudSeen& h : g_huds) {
    if (h.hud == self) {
      if (h.restarts != g_restarts) {
        h.restarts = g_restarts;
        const int shown = Rd<int>(self + flag);
        const uintptr_t drop = Rd<uintptr_t>(self + 0xA8);
        const int dropping = drop ? Rd<int>(drop + 0x34) : 0;
        *reinterpret_cast<int*>(self + flag) = 0;
        if (drop && dropping) *reinterpret_cast<int*>(drop + 0x34) = 0;
        patch::Log("restart: HUD %08x final-lap text re-armed (was shown %d, dropping %d)",
                   static_cast<unsigned>(self), shown, dropping);
      }
      return;
    }
    if (!free_slot && !h.hud) free_slot = &h;
  }
  if (!free_slot) {  // forget the oldest
    for (int i = 1; i < 8; ++i) g_huds[i - 1] = g_huds[i];
    free_slot = &g_huds[7];
  }
  *free_slot = {self, g_restarts};
  patch::Log("restart: HUD %08x seen (final-lap flag +%x = %d)", static_cast<unsigned>(self),
             static_cast<unsigned>(flag), Rd<int>(self + flag));
}

using HudUpdateFn = int(__thiscall*)(uintptr_t self, int arg);
int __fastcall HudUpdateA(uintptr_t self, void*, int arg) {
  HudRestartCheck(self, 0xA4);
  return reinterpret_cast<HudUpdateFn>(0x405C10)(self, arg);
}
int __fastcall HudUpdateB(uintptr_t self, void*, int arg) {
  HudRestartCheck(self, 0xAC);
  return reinterpret_cast<HudUpdateFn>(0x50A750)(self, arg);
}


void LogGameMode(const char* what) {
  uintptr_t g = Game();
  if (!g) return;
  patch::Log("restart: %s mode2d70=%d mode2d74=%d players=%d ai=%d gates=%d/%d seed=%d ev55c=%08x "
             "ev560=%08x ev568=%08x",
             what, Rd<int>(g + 0x2d70), Rd<int>(g + 0x2d74), Rd<int>(g + 0x18), Rd<int>(g + 0x2d94),
             Rd<uint8_t>(g + 0x2d9c), Rd<uint8_t>(g + 0x2d9d), Rd<int>(g + 0x2da0),
             static_cast<unsigned>(Rd<uintptr_t>(g + 0x55c)),
             static_cast<unsigned>(Rd<uintptr_t>(g + 0x560)),
             static_cast<unsigned>(Rd<uintptr_t>(g + 0x568)));
}

void LogVec(char*& p, char* end, const char* name, uintptr_t a) {
  int n = std::snprintf(p, end - p, " %s=(%.2f %.2f %.2f)", name, Rd<float>(a),
                      Rd<float>(a + 4), Rd<float>(a + 8));
  if (n > 0) p += (n < end - p) ? n : (end - p - 1);
}

void LogFloats(const char* tag, uintptr_t a, int from, int to) {
  char buf[2048];
  char* p = buf;
  char* end = buf + sizeof(buf);
  int n = std::snprintf(p, end - p, "restart:   %s %08x +%x:", tag, static_cast<unsigned>(a), from);
  if (n > 0) p += (n < end - p) ? n : (end - p - 1);
  for (int off = from; off < to && p < end - 16; off += 4) {
    n = std::snprintf(p, end - p, " %.2f", Rd<float>(a + off));
    if (n > 0) p += (n < end - p) ? n : (end - p - 1);
  }
  patch::Log("%s", buf);
}

// Raw dwords, 64 per line, for diffing a fresh start against a restart.
void LogHex(const char* tag, uintptr_t a, int from, int to) {
  for (int off = from; off < to; off += 64 * 4) {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "restart:   hex %s %08x +%x:", tag, static_cast<unsigned>(a), off);
    for (int k = 0; k < 64 && off + 4 * k < to && n > 0 && n < static_cast<int>(sizeof(buf)) - 12; ++k)
      n += std::snprintf(buf + n, sizeof(buf) - n, " %08x", Rd<unsigned>(a + off + 4 * k));
    patch::Log("%s", buf);
  }
}

// Deep dump at chosen frames: the first two bikes and the engine sound
// manager's voice records.
void LogDeep(uintptr_t race) {
  auto next = reinterpret_cast<NextBikeFn>(kNextBike);
  int idx = 0;
  for (int i = 0; i < 2; ++i) {
    uintptr_t bike = next(race, nullptr, &idx);
    if (!bike) break;
    LogHex(i == 0 ? "bike0" : "bike1", bike, 0x13c, 0x1440);
  }
  const uintptr_t mgr = Rd<uintptr_t>(race + 0x44);
  if (mgr && Rd<uintptr_t>(mgr) == 0x55C990) {
    LogHex("sndmgr", mgr, 0x0, 0x98);
    LogHex("voices", mgr, 0x98, 0x98 + 11 * 0x54);
    LogHex("sndmgr2", mgr, 0x434, 0x460);
    LogHex("sndmgr3", mgr, 0x1200, 0x12b0);
  }
}

void LogBikes(uintptr_t race, bool full) {
  auto next = reinterpret_cast<NextBikeFn>(kNextBike);
  int idx = 0;
  for (int guard = 0; guard < 64; ++guard) {
    int i = idx;
    uintptr_t bike = next(race, nullptr, &idx);
    if (!bike) break;
    char buf[1024];
    char* p = buf;
    char* end = buf + sizeof(buf);
    int n = std::snprintf(p, end - p, "restart:  bike%d %08x rst=%d thr=%d brk=%d spd=%.2f", i,
                        static_cast<unsigned>(bike), Rd<uint8_t>(bike + 0x109), Rd<uint8_t>(bike + 0x478),
                        Rd<uint8_t>(bike + 0x479), Rd<float>(bike + 0xb8));
    if (n > 0) p += (n < end - p) ? n : (end - p - 1);
    LogVec(p, end, "pos", bike + 0x0c);
    LogVec(p, end, "f88", bike + 0x88);
    LogVec(p, end, "f94", bike + 0x94);
    LogVec(p, end, "fa0", bike + 0xa0);
    LogVec(p, end, "gpos", bike + 0x10c);
    LogVec(p, end, "gdir", bike + 0x118);
    patch::Log("%s", buf);
    if (full) {
      LogFloats("b", bike, 0x0c, 0x130);
      uintptr_t phys = Rd<uintptr_t>(bike + 0x3bc);
      if (phys) LogFloats("p", phys, 0x80, 0x140);
    }
  }
}

void __fastcall GridSlotHook(uintptr_t self, void*, float* pos, float* dir, uintptr_t event, int slot) {
  uint8_t cached = Rd<uint8_t>(self + 0x188);
  g_grid_slot(self, nullptr, pos, dir, event, slot);
  static int s_slot0 = 0;
  if (slot == 0 && ++s_slot0 > 3) return;  // the start camera asks every frame
  if (++g_grid_calls > 400) return;
  uintptr_t ret = 0;
#if defined(_MSC_VER)
  ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
  ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
  char buf[512];
  char* p = buf;
  char* end = buf + sizeof(buf);
  int n = std::snprintf(p, end - p, "restart: grid from %08x race=%08x ev=%08x slot=%d cached=%d",
                      static_cast<unsigned>(ret), static_cast<unsigned>(self),
                      static_cast<unsigned>(event), slot, cached);
  if (n > 0) p += (n < end - p) ? n : (end - p - 1);
  LogVec(p, end, "pos", reinterpret_cast<uintptr_t>(pos));
  LogVec(p, end, "dir", reinterpret_cast<uintptr_t>(dir));
  LogVec(p, end, "base", self + 0x164);
  LogVec(p, end, "step", self + 0x170);
  n = std::snprintf(p, end - p, " nbikes=%d", Rd<int>(self + 0x158));
  if (n > 0) p += (n < end - p) ? n : (end - p - 1);
  if (event) {
    int wp = Rd<int>(event + 0xac);
    n = std::snprintf(p, end - p, " ev.wp=%d", wp);
    if (n > 0) p += (n < end - p) ? n : (end - p - 1);
    if (wp >= 0 && wp < 36) {
      LogVec(p, end, "rec.pos", event + 0xc0 + 24 * wp);
      LogVec(p, end, "rec.dir", event + 0xcc + 24 * wp);
    }
  }
  patch::Log("%s", buf);
  if (g_frame < 0 || g_race != self) {
    g_race = self;
    g_frame = 0;
    g_phase = "load";
  }
}

// Vehicle "place" (vtable +0x84, Vehicle 0x52ca80, KrustyBike 0x48f0e0):
// (pos*, forward*, up*, velocity*, keep_height, reset_arg). It resets the
// bike, sets its forward and up vectors and position, and with keep_height
// 0 settles it on the ground. This is what the restart is missing: the
// bike's own reset only moves it to bike+0x10c and leaves it turned the way
// it was when the race was paused.
using PlaceFn = int(__fastcall*)(uintptr_t self, void* edx, const float* pos, const float* fwd,
                                 const float* up, const float* vel, int keep_height, int reset_arg);
constexpr int kPlaceSlot = 0x84 / 4;

bool IsKnownPlace(uintptr_t fn) { return fn == 0x48F0E0 || fn == 0x407870 || fn == 0x52CA80; }

// Grid spots handed out at the last restart, checked after the game's first
// update: the game's own reset (bike+0x109) runs a respawn search
// (KrustyBike 0x492cc0 -> 0x4b5640) from the grid spot that moves a bike
// off it when the spot touches scenery (enduro: the boxes by the start), and
// turns it to face the track there. At race load no search runs, so the
// bikes stay where the grid put them; after a restart we put them back.
struct GridSpot {
  uintptr_t bike;
  float pos[3], fwd[3];
};
GridSpot g_spots[64];
int g_nspots = 0;
uintptr_t g_spot_race = 0;

void PlaceBikesOnGrid(uintptr_t race) {
  g_nspots = 0;
  g_spot_race = race;
  auto next = reinterpret_cast<NextBikeFn>(kNextBike);
  int idx = 0;
  int placed = 0, skipped = 0;
  for (int guard = 0; guard < 64; ++guard) {
    uintptr_t bike = next(race, nullptr, &idx);
    if (!bike) break;
    uintptr_t vt = Rd<uintptr_t>(bike);
    uintptr_t place = Rd<uintptr_t>(vt + 4 * kPlaceSlot);
    if (!Rd<uint8_t>(bike + 0x109) || !IsKnownPlace(place)) {
      ++skipped;
      continue;
    }
    float pos[3], fwd[3];
    std::memcpy(pos, reinterpret_cast<void*>(bike + 0x10c), sizeof(pos));
    std::memcpy(fwd, reinterpret_cast<void*>(bike + 0x118), sizeof(fwd));
    fwd[1] = 0.0f;
    float len = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (!(len > 1e-4f)) {
      ++skipped;
      continue;
    }
    fwd[0] /= len;
    fwd[2] /= len;
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float vel[3] = {0.0f, 0.0f, 0.0f};
    reinterpret_cast<PlaceFn>(place)(bike, nullptr, pos, fwd, up, vel, 0, 0);
    if (g_nspots < 64) {
      GridSpot& g = g_spots[g_nspots++];
      g.bike = bike;
      std::memcpy(g.pos, reinterpret_cast<void*>(bike + 0x0c), sizeof(g.pos));  // settled on the ground
      std::memcpy(g.fwd, fwd, sizeof(g.fwd));
    }
    // Place settles the bike on the ground and clears the pending reset.
    // Hand the settled spot back to the game's own reset (bike+0x109), which
    // also resets the rider and the camera on the next update.
    std::memcpy(reinterpret_cast<void*>(bike + 0x10c), reinterpret_cast<void*>(bike + 0x0c), 12);
    *reinterpret_cast<uint8_t*>(bike + 0x109) = 1;
    ++placed;
  }
  patch::Log("restart: placed %d bikes on the grid (%d skipped)", placed, skipped);
}

// After the game's first update following a restart: bikes its respawn
// search moved off their grid spot go back, facing the grid direction.
void KeepBikesOnGrid(uintptr_t race) {
  if (race != g_spot_race || !g_nspots) return;
  int moved = 0;
  for (int i = 0; i < g_nspots; ++i) {
    const GridSpot& g = g_spots[i];
    const uintptr_t bike = g.bike;
    const uintptr_t place = Rd<uintptr_t>(Rd<uintptr_t>(bike) + 4 * kPlaceSlot);
    if (!IsKnownPlace(place)) continue;
    const float dx = Rd<float>(bike + 0x0c) - g.pos[0], dz = Rd<float>(bike + 0x14) - g.pos[2];
    const float hx = Rd<float>(bike + 0x88), hz = Rd<float>(bike + 0x90);
    const float hl = std::sqrt(hx * hx + hz * hz);
    const float dot = hl > 1e-4f ? (hx * g.fwd[0] + hz * g.fwd[2]) / hl : 0.0f;
    if (dx * dx + dz * dz < 0.25f * 0.25f && dot > 0.996f) continue;
    if (GetConfig().restart_log)
      patch::Log("restart: bike %08x moved by the game's respawn search to (%.2f %.2f %.2f) heading (%.2f %.2f), "
                 "%.2f m from its grid spot; put back", static_cast<unsigned>(bike), Rd<float>(bike + 0x0c),
                 Rd<float>(bike + 0x10), Rd<float>(bike + 0x14), hx, hz, std::sqrt(dx * dx + dz * dz));
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float vel[3] = {0.0f, 0.0f, 0.0f};
    reinterpret_cast<PlaceFn>(place)(bike, nullptr, g.pos, g.fwd, up, vel, 0, 0);
    ++moved;
  }
  patch::Log("restart: after the first update %d of %d bikes were off their grid spot and were put back", moved,
             g_nspots);
  g_nspots = 0;
}

// The race's engine sound manager (BikeRace+0x44, vtable 0x55c990, update
// 0x4e84a0) keeps a voice record per bike (stride 0x54 at +0x98) with its
// engine sound state machine. The constructor sets +0x1208 so the first
// update re-initialises the records (0x4e81a0) and starts the idle loops;
// Restart Race never does, so bikes that were driving kept their driving
// engine sound through the countdown. Ask for the same re-initialisation,
// and free the four near-voice slots (+0x44c) the old records held.
constexpr uintptr_t kEngineSoundVtable = 0x55C990;

void ResetEngineSounds(uintptr_t race) {
  const uintptr_t mgr = Rd<uintptr_t>(race + 0x44);
  if (!mgr || Rd<uintptr_t>(mgr) != kEngineSoundVtable) {
    patch::Log("restart: engine sound manager not found (race+0x44 = %08x, vtable %08x), engine sounds not reset",
               static_cast<unsigned>(mgr), static_cast<unsigned>(Rd<uintptr_t>(mgr)));
    return;
  }
  for (int i = 0; i < 4; ++i) *reinterpret_cast<int*>(mgr + 0x44c + 4 * i) = 0;
  *reinterpret_cast<int*>(mgr + 0x1208) = 1;
  patch::Log("restart: engine sounds reset (manager %08x, %d voices)", static_cast<unsigned>(mgr),
             Rd<int>(mgr + 0x94));
}

// Each bike's throttle (+0x478) and brake (+0x479) flags. The AI driver
// (0x4144ed/0x41451b) only writes them once the race is running and the
// player's come from input, so after Restart Race an AI bike that was on
// the throttle keeps +0x478 = 1 through the countdown. The engine sound
// state machine (0x4e8a3c) reads it and picks the driving sound. A fresh
// race has them zeroed by the bike constructor (0x52acaf); do the same.
void ClearBikeControls(uintptr_t race) {
  auto next = reinterpret_cast<NextBikeFn>(kNextBike);
  int idx = 0, cleared = 0;
  for (int guard = 0; guard < 64; ++guard) {
    uintptr_t bike = next(race, nullptr, &idx);
    if (!bike) break;
    uint8_t* ctl = reinterpret_cast<uint8_t*>(bike + 0x478);
    if (ctl[0] || ctl[1]) ++cleared;
    ctl[0] = 0;
    ctl[1] = 0;
  }
  patch::Log("restart: throttle/brake cleared (%d bikes had them on)", cleared);
}

void __fastcall RestartHook(uintptr_t self, void*, int a, int b, int c) {
  ++g_restarts;
  if (!GetConfig().restart_log) {
    g_restart(self, nullptr, a, b, c);
    if (GetConfig().restart_fix) {
      PlaceBikesOnGrid(self);
      ClearBikeControls(self);
      ResetEngineSounds(self);
    }
    return;
  }
  patch::Log("restart: Restart Race race=%08x args=%d,%d,%d nbikes=%d", static_cast<unsigned>(self), a, b,
             c, Rd<int>(self + 0x158));
  LogGameMode("before");
  patch::Log("restart: bikes before restart");
  LogBikes(self, true);
  g_phase = "restart";
  g_race = self;
  g_frame = 0;
  g_grid_calls = 0;
  g_restart(self, nullptr, a, b, c);
  patch::Log("restart: bikes right after restart");
  LogBikes(self, true);
  if (GetConfig().restart_fix) {
    ClearBikeControls(self);
    ResetEngineSounds(self);
    PlaceBikesOnGrid(self);
    patch::Log("restart: bikes after placing them");
    LogBikes(self, true);
  }
}

// ---- RestartLog write watch ----
// For the first frames after a race load and after a restart, debug
// registers watch a few bike fields (player and second bike position z and
// heading, a third bike's +0xb8). Each write is logged once per writing
// instruction with the value and the code addresses found on the stack, so
// we can see which game code moves the bikes or changes their state.
constexpr DWORD kArmCode = 0xE0DB0001;
uintptr_t g_watch_addr[4] = {};
const char* g_watch_name[4] = {};
bool g_watch_on = false;
struct WatchHit {
  uintptr_t eip;
  int slot;
  unsigned count;
};
WatchHit g_hits[64];
int g_nhits = 0;
PVOID g_veh = nullptr;

LONG CALLBACK WatchHandler(PEXCEPTION_POINTERS e) {
  CONTEXT* c = e->ContextRecord;
  if (e->ExceptionRecord->ExceptionCode == kArmCode) {
    c->Dr0 = g_watch_on ? g_watch_addr[0] : 0;
    c->Dr1 = g_watch_on ? g_watch_addr[1] : 0;
    c->Dr2 = g_watch_on ? g_watch_addr[2] : 0;
    c->Dr3 = g_watch_on ? g_watch_addr[3] : 0;
    DWORD dr7 = 0;
    if (g_watch_on)
      for (int i = 0; i < 4; ++i)
        if (g_watch_addr[i]) dr7 |= (1u << (2 * i)) | (0xDu << (16 + 4 * i));  // local enable, write, 4 bytes
    c->Dr7 = dr7;
    c->Dr6 = 0;
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD dr6 = static_cast<DWORD>(c->Dr6);
  if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  for (int i = 0; i < 4; ++i) {
    if (!(dr6 & (1u << i))) continue;
    const uintptr_t eip = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    WatchHit* h = nullptr;
    for (int k = 0; k < g_nhits; ++k)
      if (g_hits[k].eip == eip && g_hits[k].slot == i) h = &g_hits[k];
    if (!h && g_nhits < 64) {
      h = &g_hits[g_nhits++];
      *h = {eip, i, 0};
    }
    if (h && h->count++ < 2) {
      char buf[512];
      int n = std::snprintf(buf, sizeof(buf), "restart: watch %s frame %d: %s written %.3f at %08x (after the write); stack:",
                            g_phase, g_frame, g_watch_name[i], Rd<float>(g_watch_addr[i]), static_cast<unsigned>(eip));
      const uintptr_t sp = c->Esp;
      int found = 0;
      for (int k = 0; k < 160 && found < 8 && n > 0 && n < static_cast<int>(sizeof(buf)) - 12; ++k) {
        const uintptr_t v = Rd<uintptr_t>(sp + 4 * k);
        if (v >= 0x401000 && v < 0x553000) {
          n += std::snprintf(buf + n, sizeof(buf) - n, " %08x", static_cast<unsigned>(v));
          ++found;
        }
      }
      patch::Log("%s", buf);
    }
  }
  c->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmWatch(uintptr_t race, bool on) {
  if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &WatchHandler);
  if (!g_veh) return;
  if (on) {
    auto next = reinterpret_cast<NextBikeFn>(kNextBike);
    int idx = 0;
    uintptr_t b[3] = {};
    for (int i = 0; i < 3; ++i) b[i] = next(race, nullptr, &idx);
    if (!b[0] || !b[1] || !b[2]) return;
    g_watch_addr[0] = b[0] + 0x14; g_watch_name[0] = "bike0 pos z";
    g_watch_addr[1] = b[0] + 0x90; g_watch_name[1] = "bike0 heading z";
    g_watch_addr[2] = b[1] + 0x14; g_watch_name[2] = "bike1 pos z";
    g_watch_addr[3] = b[2] + 0xb8; g_watch_name[3] = "bike2 +b8";
    g_nhits = 0;
  } else {
    for (int i = 0; i < g_nhits; ++i)
      patch::Log("restart: watch %s summary: %s written %u times at %08x", g_phase, g_watch_name[g_hits[i].slot],
                 g_hits[i].count, static_cast<unsigned>(g_hits[i].eip));
  }
  g_watch_on = on;
  RaiseException(kArmCode, 0, 0, nullptr);
  patch::Log("restart: write watch %s (%s)", on ? "armed" : "off", g_phase);
}

void __fastcall UpdateHook(uintptr_t self, void*, int arg) {
  if (!GetConfig().restart_log) {
    const bool check = g_nspots > 0 && self == g_spot_race;
    g_update(self, nullptr, arg);
    if (check) KeepBikesOnGrid(self);
    return;
  }
  bool watch = g_frame >= 0 && self == g_race;
  int f = watch ? g_frame : -1;
  if (watch && (f == 0 || f == 1 || f == 2 || f == 5 || f == 30)) {
    patch::Log("restart: %s frame %d (before update)", g_phase, f);
    if (f == 0) LogGameMode(g_phase);
    LogBikes(self, f <= 1);
  }
  if (watch && f == 0) {
    ArmWatch(self, true);
    SoundTraceStart(g_phase, 4000);
  }
  const bool check = g_nspots > 0 && self == g_spot_race;
  g_update(self, nullptr, arg);
  if (check) {
    patch::Log("restart: bikes after the game's first update");
    LogBikes(self, false);
    KeepBikesOnGrid(self);
  }
  if (watch && f == 3) ArmWatch(self, false);
  if (watch && (f == 0 || f == 1 || f == 5 || f == 30 || f == 90 || f == 200)) {
    patch::Log("restart: %s frame %d (after update)", g_phase, f);
    LogBikes(self, f <= 1);
    if (f == 1 || f == 30 || f == 90) LogDeep(self);
  }
  if (watch) {
    if (++g_frame > 200) g_frame = -1;
  }
}

// Copies the replaced entry bytes into an executable trampoline that jumps
// back past them.
void* MakeTrampoline(uintptr_t addr, const uint8_t* entry, size_t len) {
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return nullptr;
  std::memcpy(t, entry, len);
  t[len] = 0xE9;
  int32_t rel = static_cast<int32_t>((addr + len) - (reinterpret_cast<uintptr_t>(t) + len + 5));
  std::memcpy(t + len + 1, &rel, 4);
  FlushInstructionCache(GetCurrentProcess(), t, len + 5);
  return t;
}

}  // namespace

void ApplyRestartFixes() {
  const Config& c = GetConfig();
  if (!c.restart_fix && !c.restart_log) return;
  if (!patch::Matches(kRestart, kRestartEntry, sizeof(kRestartEntry))) {
    patch::Log("restart: code differs, not applied");
    return;
  }
  g_restart = reinterpret_cast<RestartFn>(MakeTrampoline(kRestart, kRestartEntry, sizeof(kRestartEntry)));
  if (!g_restart) return;
  patch::Jump(kRestart, reinterpret_cast<uintptr_t>(&RestartHook), sizeof(kRestartEntry));
  patch::Log("restart: Restart Race fix %s", c.restart_fix ? "applied" : "off");
  if (c.restart_fix) {
    int hooked = 0;
    for (const HudHook& h : kHudHooks) {
      if (Rd<uintptr_t>(h.slot) != h.fn) {
        patch::Log("restart: HUD slot %08x holds %08x, final-lap re-arm not hooked there",
                   static_cast<unsigned>(h.slot), Rd<unsigned>(h.slot));
        continue;
      }
      void* fn = h.flag == 0xA4 ? reinterpret_cast<void*>(&HudUpdateA) : reinterpret_cast<void*>(&HudUpdateB);
      patch::Write(h.slot, &fn, sizeof(fn));
      ++hooked;
    }
    patch::Log("restart: FINAL LAP text re-armed after restarts (%d HUD slots hooked)", hooked);
  }
  uintptr_t slot = kBikeRaceVtable + 4 * kUpdateSlot;
  if (Rd<uintptr_t>(slot) != 0x41E0B0) {
    patch::Log("restart: race update slot differs, grid check and logging not applied");
    return;
  }
  g_update = reinterpret_cast<UpdateFn>(Rd<uintptr_t>(slot));
  uintptr_t hook = reinterpret_cast<uintptr_t>(&UpdateHook);
  patch::Write(slot, &hook, sizeof(hook));
  if (c.restart_fix) patch::Log("restart: bikes moved off the grid by the game's respawn search are put back");
  if (!c.restart_log) return;

  if (!patch::Matches(kGridSlot, kGridSlotEntry, sizeof(kGridSlotEntry))) {
    patch::Log("restart: grid code differs, grid logging not applied");
  } else {
    g_grid_slot = reinterpret_cast<GridSlotFn>(MakeTrampoline(kGridSlot, kGridSlotEntry, sizeof(kGridSlotEntry)));
    if (g_grid_slot) patch::Jump(kGridSlot, reinterpret_cast<uintptr_t>(&GridSlotHook), sizeof(kGridSlotEntry));
  }
  patch::Log("restart: logging applied");
}
