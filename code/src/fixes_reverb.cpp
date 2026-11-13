// EAX reverb on outdoor tracks (BajaReverb experiment).
//
// The game uses EAX 1.0 only. At race start (0x4e8463) and from a command
// toggle (0x4e934f) it sets DSPROPERTY_EAX_ALL through 0x4c3490 (thiscall on
// the sound object, one argument: EAX_REVERBPROPERTIES {environment, volume,
// decay time, damping}). Supercross and Moto-Tag get Arena; every other
// event gets {Mountains, 0, 0, 0}, meant as "reverb off". OpenAL Soft (behind
// DSOAL) rejects that whole set because decay time 0 is below its 0.1 s
// minimum, so the previous reverb stays.
//
// BajaReverb=<EAX 1.0 preset name> replaces those "off" values with the
// preset's values in Baja events (game+0x2d78 == 1). Every EAX set the game
// makes (ALL and ENVIRONMENT) is logged with the gate flags and the result.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "config.h"
#include "patch.h"

namespace {

struct EaxProps {
  DWORD environment;
  float volume;
  float decay;
  float damping;
};

struct Preset {
  const char* name;
  EaxProps p;
};

// EAX 1.0 SDK presets (EAX_PRESET_*).
constexpr Preset kPresets[] = {
    {"Generic", {0, 0.5f, 1.493f, 0.5f}},
    {"PaddedCell", {1, 0.25f, 0.1f, 0.0f}},
    {"Room", {2, 0.417f, 0.4f, 0.666f}},
    {"Bathroom", {3, 0.653f, 1.499f, 0.166f}},
    {"LivingRoom", {4, 0.208f, 0.478f, 0.0f}},
    {"StoneRoom", {5, 0.5f, 2.309f, 0.888f}},
    {"Auditorium", {6, 0.403f, 4.279f, 0.5f}},
    {"ConcertHall", {7, 0.5f, 3.961f, 0.5f}},
    {"Cave", {8, 0.5f, 2.886f, 1.304f}},
    {"Arena", {9, 0.361f, 7.284f, 0.332f}},
    {"Hangar", {10, 0.5f, 10.0f, 0.3f}},
    {"CarpetedHallway", {11, 0.153f, 0.259f, 2.0f}},
    {"Hallway", {12, 0.361f, 1.493f, 0.0f}},
    {"StoneCorridor", {13, 0.444f, 2.697f, 0.638f}},
    {"Alley", {14, 0.25f, 1.752f, 0.776f}},
    {"Forest", {15, 0.111f, 3.145f, 0.472f}},
    {"City", {16, 0.111f, 2.767f, 0.224f}},
    {"Mountains", {17, 0.194f, 7.841f, 0.472f}},
    {"Quarry", {18, 1.0f, 1.499f, 0.5f}},
    {"Plain", {19, 0.097f, 2.767f, 0.224f}},
    {"ParkingLot", {20, 0.208f, 1.652f, 1.5f}},
    {"SewerPipe", {21, 0.652f, 2.886f, 0.25f}},
    {"Underwater", {22, 1.0f, 1.499f, 0.0f}},
    {"Drugged", {23, 0.875f, 8.392f, 1.388f}},
    {"Dizzy", {24, 0.139f, 17.234f, 0.666f}},
    {"Psychotic", {25, 0.486f, 7.563f, 0.806f}},
};

const char* EnvName(DWORD e) {
  return e < sizeof(kPresets) / sizeof(kPresets[0]) ? kPresets[e].name : "?";
}

constexpr uintptr_t kGame = 0x5743ac;         // game object pointer
constexpr uintptr_t kSetAll = 0x4c3490;       // sound: Set EAX ALL (thiscall, props*)
constexpr uintptr_t kSetEnv = 0x4c3450;       // sound: Set EAX ENVIRONMENT (thiscall, DWORD)
constexpr uintptr_t kRaceStartCall = 0x4e8463;
constexpr uintptr_t kToggleCall = 0x4e934f;
constexpr uintptr_t kInitEnvCall = 0x4c3247;
constexpr uintptr_t kZoneEnvCall = 0x403758;

using SetAllFn = int(__thiscall*)(uintptr_t self, const EaxProps* p);
using SetEnvFn = int(__thiscall*)(uintptr_t self, DWORD env);

const Preset* g_baja = nullptr;  // BajaReverb preset, null = game's own values
int g_env_logs = 0;              // ENVIRONMENT sets logged (zone updates could be per frame)

int Event(int off) {
  const uintptr_t g = *reinterpret_cast<uintptr_t*>(kGame);
  return g ? *reinterpret_cast<int*>(g + off) : -1;
}

// Why a set may never reach DirectSound: the sound object's gates in 0x4c3490.
void Gates(uintptr_t self, char* out, size_t n) {
  const uint8_t enabled = *reinterpret_cast<uint8_t*>(self + 0x2c);
  const uint8_t flags = *reinterpret_cast<uint8_t*>(self + 0x45c);
  const uintptr_t prop = *reinterpret_cast<uintptr_t*>(self + 0x474);
  wsprintfA(out, "sound on=%d eax supported=%d eax on=%d propset=%s", enabled & 1,
            (flags >> 2) & 1, (flags >> 3) & 1, prop ? "yes" : "none");
  (void)n;
}

void LogProps(const char* what, const EaxProps& p) {
  patch::Log("reverb:   %s env %lu (%s) volume %.3f decay %.3f s damping %.3f", what,
             p.environment, EnvName(p.environment), p.volume, p.decay, p.damping);
}

int SetAll(const char* who, uintptr_t self, const EaxProps* p) {
  char gates[128];
  Gates(self, gates, sizeof(gates));
  const int ev74 = Event(0x2d74), ev78 = Event(0x2d78);
  patch::Log("reverb: %s EAX ALL, event %d (2d74 %d), %s", who, ev78, ev74, gates);
  LogProps("game wants", *p);
  EaxProps use = *p;
  const bool game_off = p->environment == 17 && p->volume == 0.0f && p->decay == 0.0f;
  if (g_baja && ev78 == 1 && game_off) {
    use = g_baja->p;
    LogProps("BajaReverb sends", use);
  } else if (g_baja) {
    patch::Log("reverb:   left as is (%s)", ev78 != 1 ? "not a Baja event" : "not the game's 'off' values");
  }
  const int r = reinterpret_cast<SetAllFn>(kSetAll)(self, &use);
  patch::Log("reverb:   result %d (0 = not sent: EAX off or unsupported)", r);
  return r;
}

int __fastcall RaceStartSetAll(uintptr_t self, void*, const EaxProps* p) {
  return SetAll("race start", self, p);
}

int __fastcall ToggleSetAll(uintptr_t self, void*, const EaxProps* p) {
  return SetAll("reverb toggle", self, p);
}

int SetEnv(const char* who, uintptr_t self, DWORD env) {
  const int r = reinterpret_cast<SetEnvFn>(kSetEnv)(self, env);
  if (g_env_logs < 40) {
    ++g_env_logs;
    char gates[128];
    Gates(self, gates, sizeof(gates));
    patch::Log("reverb: %s EAX ENVIRONMENT %lu (%s), result %d, %s%s", who, env, EnvName(env), r,
               gates, g_env_logs == 40 ? " (no more ENVIRONMENT logs)" : "");
  }
  return r;
}

int __fastcall InitSetEnv(uintptr_t self, void*, DWORD env) { return SetEnv("init", self, env); }
int __fastcall ZoneSetEnv(uintptr_t self, void*, DWORD env) { return SetEnv("zone", self, env); }

bool HookCall(const char* name, uintptr_t at, uintptr_t target, const void* fn) {
  uint8_t orig[5] = {0xE8};
  const int32_t rel = static_cast<int32_t>(target - (at + 5));
  std::memcpy(orig + 1, &rel, 4);
  if (!patch::Matches(at, orig, sizeof(orig))) {
    patch::Log("reverb: %s call at %08x differs, not hooked", name, static_cast<unsigned>(at));
    return false;
  }
  patch::Call(at, fn, sizeof(orig));
  return true;
}

}  // namespace

void ApplyReverbFixes() {
  const char* want = GetConfig().baja_reverb;
  g_baja = nullptr;
  if (want[0] && lstrcmpiA(want, "Off") != 0) {
    for (const Preset& p : kPresets)
      if (lstrcmpiA(want, p.name) == 0) g_baja = &p;
    if (!g_baja) patch::Log("reverb: BajaReverb=%s is not an EAX 1.0 preset name, game's values kept", want);
  }
  int hooked = 0;
  hooked += HookCall("race start", kRaceStartCall, kSetAll, reinterpret_cast<const void*>(&RaceStartSetAll));
  hooked += HookCall("toggle", kToggleCall, kSetAll, reinterpret_cast<const void*>(&ToggleSetAll));
  hooked += HookCall("init env", kInitEnvCall, kSetEnv, reinterpret_cast<const void*>(&InitSetEnv));
  hooked += HookCall("zone env", kZoneEnvCall, kSetEnv, reinterpret_cast<const void*>(&ZoneSetEnv));
  if (g_baja) {
    patch::Log("reverb: BajaReverb=%s (env %lu, volume %.3f, decay %.3f s, damping %.3f), %d/4 calls hooked",
               g_baja->name, g_baja->p.environment, g_baja->p.volume, g_baja->p.decay, g_baja->p.damping,
               hooked);
  } else {
    patch::Log("reverb: BajaReverb=Off (game's values), EAX calls logged, %d/4 calls hooked", hooked);
  }
}
