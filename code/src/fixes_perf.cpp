// CPU hot spots in the game's own code found with Profile=1 (perf-1 run):
//
//  - 0x4c41d0, the game's millisecond clock, calls timeBeginPeriod(1),
//    timeGetTime and timeEndPeriod(1) every time (96 call sites, ~10% of the
//    game thread). Each period change reprograms the system timer. We keep
//    the period at 1 ms for the whole run and read the clock from QPC.
//  - 0x53b6e0, the CRT's _stricmp (381 call sites, ~7%): once the game sets
//    a locale, every differing character goes through tolower ->
//    LCMapStringA under a CRT lock. We compare with a 256-entry table made
//    from the CRT's own tolower, rebuilt if the locale changes.
#include <windows.h>
#include <mmsystem.h>

#include <cstdint>

#include "config.h"
#include "patch.h"

namespace {

// ---------------------------------------------------------------- clock

LARGE_INTEGER g_freq, g_q0;
DWORD g_ms0;

DWORD __cdecl GameMs() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  const uint64_t d = static_cast<uint64_t>(q.QuadPart - g_q0.QuadPart);
  const uint64_t f = static_cast<uint64_t>(g_freq.QuadPart);
  return g_ms0 + static_cast<DWORD>((d / f) * 1000 + (d % f) * 1000 / f);
}

void PatchClock() {
  const uint8_t original[] = {
      0x56,                                // push esi
      0x6A, 0x01,                          // push 1
      0xFF, 0x15, 0x98, 0x53, 0x55, 0x00,  // call [timeBeginPeriod]
      0xFF, 0x15, 0xA4, 0x53, 0x55, 0x00,  // call [timeGetTime]
      0x6A, 0x01,                          // push 1
      0x8B, 0xF0,                          // mov esi,eax
      0xFF, 0x15, 0x9C, 0x53, 0x55, 0x00,  // call [timeEndPeriod]
      0x8B, 0xC6, 0x5E, 0xC3,              // mov eax,esi / pop esi / ret
  };
  constexpr uintptr_t kAt = 0x4C41D0;
  if (!patch::Matches(kAt, original, sizeof(original))) {
    patch::Log("perf: game clock 0x4c41d0 differs, not replaced");
    return;
  }
  QueryPerformanceFrequency(&g_freq);
  timeBeginPeriod(1);  // for the whole run (Sleep granularity), never ended
  g_ms0 = timeGetTime();
  QueryPerformanceCounter(&g_q0);
  patch::Jump(kAt, reinterpret_cast<uintptr_t>(&GameMs), sizeof(original));
  patch::Log("perf: game clock 0x4c41d0 reads QPC (%lld Hz); timer period 1 ms set once", g_freq.QuadPart);
}

// ---------------------------------------------------------------- _stricmp

using ToLowerFn = int(__cdecl*)(int);
const ToLowerFn kCrtToLower = reinterpret_cast<ToLowerFn>(0x544163);
volatile LONG* const kLcCtype = reinterpret_cast<volatile LONG*>(0x690F6C);  // __lc_handle[LC_CTYPE]

unsigned char g_lower[256];
volatile LONG g_lower_for = -1;  // locale handle g_lower was built for
volatile LONG g_rebuilds = 0;

void BuildLower(LONG handle) {
  unsigned char t[256];
  for (int c = 0; c < 256; ++c) t[c] = static_cast<unsigned char>(kCrtToLower(c));
  for (int c = 0; c < 256; ++c) g_lower[c] = t[c];
  MemoryBarrier();
  InterlockedExchange(&g_lower_for, handle);
  if (InterlockedIncrement(&g_rebuilds) <= 8) {
    int changed = 0;
    for (int c = 0; c < 256; ++c) {
      int ascii = (c >= 'A' && c <= 'Z') ? c + 32 : c;
      if (t[c] != ascii) ++changed;
    }
    patch::Log("perf: _stricmp table built for locale handle %08lx (%d chars differ from ASCII)", handle, changed);
  }
}

int __cdecl FastStricmp(const char* a, const char* b) {
  const LONG h = *kLcCtype;
  if (h != g_lower_for) BuildLower(h);
  const auto* p = reinterpret_cast<const unsigned char*>(a);
  const auto* q = reinterpret_cast<const unsigned char*>(b);
  for (;;) {
    const unsigned char x = g_lower[*p++];
    const unsigned char y = g_lower[*q++];
    if (x != y) return x < y ? -1 : 1;
    if (x == 0) return 0;
  }
}

void PatchStricmp() {
  const uint8_t original[] = {
      0x55, 0x8B, 0xEC, 0x57, 0x56, 0x53,  // push ebp / mov ebp,esp / push edi,esi,ebx
      0x8B, 0x75, 0x0C, 0x8B, 0x7D, 0x08,  // mov esi,[ebp+0Ch] / mov edi,[ebp+8]
      0x8D, 0x05, 0x64, 0x0F, 0x69, 0x00,  // lea eax,[690F64h]
  };
  constexpr uintptr_t kAt = 0x53B6E0;
  if (!patch::Matches(kAt, original, sizeof(original))) {
    patch::Log("perf: _stricmp 0x53b6e0 differs, not replaced");
    return;
  }
  patch::Jump(kAt, reinterpret_cast<uintptr_t>(&FastStricmp), 5);
  patch::Log("perf: _stricmp 0x53b6e0 replaced (locale handle now %08lx)", *kLcCtype);
}

}  // namespace

void ApplyPerfFixes() {
  if (!GetConfig().fast_crt) {
    patch::Log("perf: FastCrt=0, game clock and _stricmp left as they are");
    return;
  }
  PatchClock();
  PatchStricmp();
}
