// RacePower: Windows' High performance power plan while racing.
//
// Found on Iron Mountain (2026-11-13, i7-6850K, Balanced plan): every few
// frames took ~2.3x as long with the same work. The game thread was never
// descheduled; the CPU clock was down to 1200-1500 MHz (of 3600). On
// Balanced, Windows lowers the clock while the game waits for VSync, and
// the next frame then runs slowly. On High performance the clock stayed at
// 3601 MHz and the slow frames went from ~20 to 0-1 per 5 seconds.
// (Tried instead: an idle-priority busy thread. Clocks only rose to ~1900
// MHz on average and 5-20 slow frames per 5 s remained, so it was dropped.)
//
// RacePower=1 (default) switches the active plan to High performance when a
// race starts and back 10 seconds after it stops (so pause and restart
// don't flip it). 2 = from the first frame until the game closes. 0 = never.
// It is left alone when the plan is already High performance or Ultimate
// Performance, on battery, or when Windows has no High performance plan.
//
// The plan we replaced is kept in HKCU\Software\mcm2fix PowerPlanRestore
// while switched, so a crash or a killed game is put right at the next
// start. We only switch back if the plan is still the one we set (the user
// may have changed it meanwhile). The calls are looked up at run time
// (Vista+; XP has no power plans).
#include <windows.h>

#include <cstdio>

#include "config.h"
#include "patch.h"

namespace power {
namespace {

const GUID kHighPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
const char kKey[] = "Software\\mcm2fix";
const char kValue[] = "PowerPlanRestore";
const DWORD kRestoreAfterMs = 10000;

typedef DWORD(WINAPI* GetActiveFn)(HKEY, GUID**);
typedef DWORD(WINAPI* SetActiveFn)(HKEY, const GUID*);

GetActiveFn g_get = nullptr;
SetActiveFn g_set = nullptr;
bool g_loaded = false;
bool g_switched = false;  // we set High performance and saved the old plan
bool g_gave_up = false;   // switching failed or is not wanted on this PC
GUID g_saved = {};
DWORD g_last_race = 0;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

void GuidText(const GUID& g, char* out, size_t n) {
  std::snprintf(out, n, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1),
                g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
                g.Data4[7]);
}

const char* PlanName(const GUID& g) {
  static const GUID kBalanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
  static const GUID kSaver = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
  if (IsEqualGUID(g, kHighPerf)) return "High performance";
  if (IsEqualGUID(g, kUltimate)) return "Ultimate Performance";
  if (IsEqualGUID(g, kBalanced)) return "Balanced";
  if (IsEqualGUID(g, kSaver)) return "Power saver";
  return "custom plan";
}

bool Load() {
  if (g_loaded) return g_get && g_set;
  g_loaded = true;
  if (HMODULE m = LoadLibraryA("powrprof.dll")) {
    g_get = reinterpret_cast<GetActiveFn>(reinterpret_cast<void*>(GetProcAddress(m, "PowerGetActiveScheme")));
    g_set = reinterpret_cast<SetActiveFn>(reinterpret_cast<void*>(GetProcAddress(m, "PowerSetActiveScheme")));
  }
  if (!g_get || !g_set) patch::Log("power: no power plan calls (Windows XP?); RacePower does nothing");
  return g_get && g_set;
}

bool Active(GUID* out) {
  GUID* p = nullptr;
  if (g_get(nullptr, &p) != ERROR_SUCCESS || !p) return false;
  *out = *p;
  LocalFree(p);
  return true;
}

void SaveRestore(const GUID* g) {
  HKEY k;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
    return;
  if (g) RegSetValueExA(k, kValue, 0, REG_BINARY, reinterpret_cast<const BYTE*>(g), sizeof(GUID));
  else RegDeleteValueA(k, kValue);
  RegCloseKey(k);
}

bool ReadRestore(GUID* g) {
  HKEY k;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
  DWORD type = 0, size = sizeof(GUID);
  const bool ok = RegQueryValueExA(k, kValue, nullptr, &type, reinterpret_cast<BYTE*>(g), &size) == ERROR_SUCCESS &&
                  type == REG_BINARY && size == sizeof(GUID);
  RegCloseKey(k);
  return ok;
}

// A plan left switched by a run that crashed or was killed.
void RestoreLeftover() {
  GUID saved;
  if (!ReadRestore(&saved)) return;
  GUID now = {};
  char a[40], b[40];
  GuidText(saved, a, sizeof(a));
  if (Active(&now) && IsEqualGUID(now, kHighPerf)) {
    const DWORD r = g_set(nullptr, &saved);
    patch::Log("power: last run left High performance on; plan set back to %s (%s): %s (%lu)", PlanName(saved), a,
               r == ERROR_SUCCESS ? "ok" : "failed", r);
  } else {
    GuidText(now, b, sizeof(b));
    patch::Log("power: last run's saved plan %s found but the plan is now %s (%s); left as it is", a, PlanName(now), b);
  }
  SaveRestore(nullptr);
}

void Switch() {
  if (g_switched || g_gave_up) return;
  SYSTEM_POWER_STATUS ps;
  if (GetSystemPowerStatus(&ps) && ps.ACLineStatus == 0) {
    static bool said = false;
    if (!said) patch::Log("power: on battery; power plan left as it is");
    said = true;
    return;  // checked again each frame
  }
  GUID now = {};
  if (!Active(&now)) {
    patch::Log("power: could not read the active power plan; RacePower off for this run");
    g_gave_up = true;
    return;
  }
  char a[40];
  GuidText(now, a, sizeof(a));
  if (IsEqualGUID(now, kHighPerf) || IsEqualGUID(now, kUltimate)) {
    patch::Log("power: plan is already %s; left as it is", PlanName(now));
    g_gave_up = true;
    return;
  }
  SaveRestore(&now);
  const DWORD r = g_set(nullptr, &kHighPerf);
  if (r != ERROR_SUCCESS) {
    SaveRestore(nullptr);
    patch::Log("power: could not switch %s (%s) to High performance (error %lu; no such plan, or not allowed); "
               "RacePower off for this run", PlanName(now), a, r);
    g_gave_up = true;
    return;
  }
  g_saved = now;
  g_switched = true;
  patch::Log("power: plan %s (%s) -> High performance", PlanName(now), a);
}

void Restore(const char* why) {
  if (!g_switched) return;
  g_switched = false;
  GUID now = {};
  if (Active(&now) && !IsEqualGUID(now, kHighPerf)) {
    patch::Log("power: %s; plan was changed to %s meanwhile, left as it is", why, PlanName(now));
  } else {
    const DWORD r = g_set(nullptr, &g_saved);
    patch::Log("power: %s; plan back to %s: %s (%lu)", why, PlanName(g_saved), r == ERROR_SUCCESS ? "ok" : "failed",
               r);
  }
  SaveRestore(nullptr);
}

// --- worker thread -------------------------------------------------------
// PowerSetActiveScheme can take ~2 s (seen 2026-11-13 on StOo's PC going back
// to Balanced: the game froze 1.7-1.9 s during the next race's countdown and
// in the menus after a race). The calls now run on this thread; the game
// thread only says which plan it wants. RacePowerAsync=0 runs them on the
// game thread as before.
HANDLE g_wake = nullptr;
HANDLE g_thread = nullptr;
volatile LONG g_want = 0;      // 1 = High performance wanted, 0 = back to the saved plan
volatile LONG g_busy = 0;      // a plan call is running on the worker
volatile LONG g_busy_since = 0;

double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

void Apply(bool high, const char* why, const char* where) {
  if (high == g_switched || (high && g_gave_up)) return;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  if (high) Switch();
  else Restore(why);
  QueryPerformanceCounter(&t1);
  patch::Log("power: %s took %.1f ms (%s)", high ? "switch to High performance" : "switch back", Ms(t0, t1), where);
}

DWORD WINAPI Worker(void*) {
  for (;;) {
    WaitForSingleObject(g_wake, INFINITE);
    const bool high = InterlockedCompareExchange(&g_want, 0, 0) != 0;
    EnterCriticalSection(&g_lock);
    if (high != g_switched && !(high && g_gave_up)) {
      InterlockedExchange(&g_busy_since, static_cast<LONG>(GetTickCount()));
      InterlockedExchange(&g_busy, 1);
      Apply(high, "race over", "power thread");
      InterlockedExchange(&g_busy, 0);
    }
    LeaveCriticalSection(&g_lock);
  }
}

bool StartWorker() {
  g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (g_wake) g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (!g_thread) {
    patch::Log("power: could not start the power thread (error %lu); plan calls run on the game thread",
               GetLastError());
    return false;
  }
  SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
  patch::Log("power: plan changes run on their own thread (RacePowerAsync=1)");
  return true;
}

}  // namespace

// Each frame from the game thread (not DllMain: loads powrprof.dll).
void Frame(bool racing) {
  const int mode = GetConfig().race_power;
  static bool started = false;
  static bool async = false;
  if (!started) {
    started = true;
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
    if (Load()) RestoreLeftover();
    patch::Log("power: RacePower=%d (%s), RacePowerAsync=%d", mode,
               mode == 2 ? "High performance while the game runs" : mode ? "High performance while racing" : "off",
               GetConfig().race_power_async);
    if (mode && g_get && g_set && GetConfig().race_power_async) async = StartWorker();
  }

  // Game-thread gap watch: logs any frame-to-frame gap over 250 ms with what
  // the power code was doing, so other stalls show up in the log too.
  {
    static LARGE_INTEGER last = {};
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (last.QuadPart) {
      const double gap = Ms(last, t);
      if (gap > 250.0) {
        const bool busy = InterlockedCompareExchange(&g_busy, 0, 0) != 0;
        patch::Log("stall: game thread %.0f ms between frames (racing %d; power call %s)", gap, racing ? 1 : 0,
                   busy ? "running on the power thread" : "not running");
      }
    }
    last = t;
  }

  if (!mode) return;
  const DWORD now = GetTickCount();
  if (!g_get || !g_set) return;
  bool want;
  if (mode == 2 || racing) {
    g_last_race = now;
    want = true;
  } else {
    want = InterlockedCompareExchange(&g_want, 0, 0) != 0 && now - g_last_race < kRestoreAfterMs;
  }
  if (async) {
    const LONG w = want ? 1 : 0;
    if (InterlockedExchange(&g_want, w) != w) {
      patch::Log("power: game thread asks for %s (racing %d)", want ? "High performance" : "the saved plan",
                 racing ? 1 : 0);
      SetEvent(g_wake);
    }
    return;
  }
  InterlockedExchange(&g_want, want ? 1 : 0);
  EnterCriticalSection(&g_lock);
  Apply(want, "race over", "game thread");
  LeaveCriticalSection(&g_lock);
}

// At exit or crash (any thread). Waits for a running plan call first.
void Shutdown(const char* why) {
  if (!g_lock_ready) return;
  EnterCriticalSection(&g_lock);
  Restore(why);
  LeaveCriticalSection(&g_lock);
}

}  // namespace power
