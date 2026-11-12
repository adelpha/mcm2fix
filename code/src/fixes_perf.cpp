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

// ---------------------------------------------------------- 3D sound commit
//
// Every simulation step the game moves its sounds (deferred) and then calls
// IDirectSound3DListener::CommitDeferredSettings (0x4c32d0, vtable +0x44),
// which waits on DirectSound's own lock and took ~60% of a step's time in
// perf-2. The commit now runs on a worker thread; the game goes on at once.
// The commit holds DirectSound's lock for 1-2 ms, and perf-5 showed the game's
// own sound calls in the next step waiting on that lock. With AsyncSound=2 the
// worker starts the commit when the frame is handed to Present, so it runs
// while the game thread waits for VSync instead of during a sim step.

using CommitFn = HRESULT(__stdcall*)(void*);
using RefFn = ULONG(__stdcall*)(void*);

// Listeners to commit (each holds a reference). The game commits from two
// places (0x4c32d0 and 0x402b70), normally on the same listener.
constexpr int kMaxPending = 4;
void* g_pending[kMaxPending] = {};
volatile LONG g_npending = 0;
CRITICAL_SECTION g_pending_cs;
HANDLE g_commit_event = nullptr;
bool g_at_present = false;  // AsyncSound=2: commit started by PerfBeforePresent
volatile LONG g_obj_commits = 0, g_obj_sync = 0;  // 0x402b70 calls, and those not queued
volatile LONG g_timeouts = 0, g_commits = 0, g_commit_us = 0, g_commit_max_us = 0, g_skipped = 0;
LARGE_INTEGER g_commit_freq;

ULONG Vcall(void* obj, int slot) {
  auto fn = reinterpret_cast<RefFn>((*reinterpret_cast<void***>(obj))[slot]);
  return fn(obj);
}

// Which DirectSound the game got (Windows' own, or a replacement such as
// DSOAL dropped next to the exe), plus a DSOAL driver if one is loaded.
void LogSoundModules() {
  const char* names[] = {"dsound.dll", "dsoal-aldrv.dll", "soft_oal.dll", "OpenAL32.dll"};
  for (const char* name : names) {
    HMODULE m = GetModuleHandleA(name);
    char path[MAX_PATH] = {};
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    if (m || name == names[0]) patch::Log("sound: %s %s%s", name, m ? "loaded from " : "not loaded", path);
  }
}

DWORD WINAPI CommitThread(void*) {
  DWORD last_log = GetTickCount();
  bool logged_modules = false;
  for (;;) {
    // The timeout keeps sound moving when nothing is presented (loading, video).
    const bool timed_out = WaitForSingleObject(g_commit_event, 50) == WAIT_TIMEOUT;
    void* todo[kMaxPending];
    int count = 0;
    EnterCriticalSection(&g_pending_cs);
    for (; count < g_npending; ++count) todo[count] = g_pending[count];
    g_npending = 0;
    LeaveCriticalSection(&g_pending_cs);
    if (count && timed_out) InterlockedIncrement(&g_timeouts);
    if (count && !logged_modules) {
      logged_modules = true;
      LogSoundModules();
    }
    for (int i = 0; i < count; ++i) {
      void* l = todo[i];
      LARGE_INTEGER a, b;
      QueryPerformanceCounter(&a);
      reinterpret_cast<CommitFn>((*reinterpret_cast<void***>(l))[0x44 / 4])(l);
      QueryPerformanceCounter(&b);
      Vcall(l, 2);  // Release
      const LONG us = static_cast<LONG>((b.QuadPart - a.QuadPart) * 1000000 / g_commit_freq.QuadPart);
      InterlockedIncrement(&g_commits);
      InterlockedExchangeAdd(&g_commit_us, us);
      if (us > g_commit_max_us) g_commit_max_us = us;
    }
    const DWORD now = GetTickCount();
    if (now - last_log >= 10000) {
      last_log = now;
      const LONG n = InterlockedExchange(&g_commits, 0), t = InterlockedExchange(&g_commit_us, 0);
      const LONG mx = InterlockedExchange(&g_commit_max_us, 0), sk = InterlockedExchange(&g_skipped, 0);
      const LONG to = InterlockedExchange(&g_timeouts, 0);
      const LONG oc = InterlockedExchange(&g_obj_commits, 0), os = InterlockedExchange(&g_obj_sync, 0);
      if (n && (GetConfig().profile || GetConfig().dd_stats))
        patch::Log("perf: 3D sound commits on the worker 10s: %ld, average %ld us, max %ld us, %ld merged, %ld started "
                   "without a Present; 0x402b70 calls %ld (%ld left on the game thread)",
                   n, t / n, mx, sk, to, oc, os);
    }
  }
}

// Queue a commit of listener l for the worker. False when the queue is full
// (then the caller commits itself).
bool QueueCommit(void* l) {
  bool queued = true;
  EnterCriticalSection(&g_pending_cs);
  int i = 0;
  while (i < g_npending && g_pending[i] != l) ++i;
  if (i < g_npending) {
    InterlockedIncrement(&g_skipped);  // already queued: one commit covers both
  } else if (g_npending < kMaxPending) {
    Vcall(l, 1);  // AddRef: kept alive until the worker has committed
    g_pending[g_npending++] = l;
  } else {
    queued = false;
  }
  LeaveCriticalSection(&g_pending_cs);
  if (queued && !g_at_present) SetEvent(g_commit_event);
  return queued;
}

HRESULT CommitNow(void* l) { return reinterpret_cast<CommitFn>((*reinterpret_cast<void***>(l))[0x44 / 4])(l); }

// The game's sound object (this of 0x4c32d0). At start-up (0x4c31e8) it only
// probes EAX when flag bit 0 is set and the driver reports hardware 3D
// buffers (+0x410 >= 1); a supported probe sets bit 2, and the EAX
// environment is only sent (0x4c3450) with bit 2 and bit 3
// (AllowEAXExtension, the options checkbox) both set.
void LogEaxState(uintptr_t self) {
  const uint8_t f = *reinterpret_cast<uint8_t*>(self + 0x45c);
  patch::Log("sound: EAX state: flags %02x (bit0 probe allowed %d, bit2 EAX supported %d, bit3 EAX option on %d), "
             "hardware 3D buffers %lu, probe buffer %p",
             f, f & 1, (f >> 2) & 1, (f >> 3) & 1, *reinterpret_cast<unsigned long*>(self + 0x410),
             *reinterpret_cast<void**>(self + 0x474));
}

int __fastcall SoundCommit(uintptr_t self) {
  static uint8_t last_flags = 0xff;
  if (*reinterpret_cast<uint8_t*>(self + 0x45c) != last_flags) {
    last_flags = *reinterpret_cast<uint8_t*>(self + 0x45c);
    LogEaxState(self);
  }
  if (!(*reinterpret_cast<uint8_t*>(self + 0x2c) & 1)) return 0;
  void* l = *reinterpret_cast<void**>(self + 0x464);
  if (!l) return 0;
  if (!QueueCommit(l)) CommitNow(l);
  return 1;
}

// 0x402b70: the sound wrappers' own commit (listener at +4), called in a loop
// over the game's sound objects (0x40376b) and after setting the distance
// factor (0x402b57). perf-6 showed it on the game thread inside DirectSound's
// registry reads. Each object is checked once to really be a 3D listener;
// anything else is committed the game's way.
const GUID kIID_Listener = {0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
void* g_checked[16] = {};
bool g_checked_ok[16] = {};

bool IsListener(void* l) {
  for (int i = 0; i < 16; ++i)
    if (g_checked[i] == l) return g_checked_ok[i];
  void* out = nullptr;
  using QiFn = HRESULT(__stdcall*)(void*, const GUID&, void**);
  const HRESULT hr = reinterpret_cast<QiFn>((*reinterpret_cast<void***>(l))[0])(l, kIID_Listener, &out);
  const bool ok = SUCCEEDED(hr) && out == l;
  if (out) Vcall(out, 2);
  static int next = 0;
  g_checked[next] = l;
  g_checked_ok[next] = ok;
  next = (next + 1) % 16;
  patch::Log("perf: sound object commit 0x402b70 on %p: %s", l,
             ok ? "3D listener, runs on the worker" : "not a 3D listener, left on the game thread");
  return ok;
}

int __fastcall ObjectCommit(uintptr_t self) {
  void* l = *reinterpret_cast<void**>(self + 4);
  if (!l) return 0;  // the original returns with eax = 0 here too
  InterlockedIncrement(&g_obj_commits);
  if (IsListener(l) && QueueCommit(l)) return 1;
  InterlockedIncrement(&g_obj_sync);
  return CommitNow(l) >= 0 ? 1 : 0;
}

void PatchObjectCommit() {
  const uint8_t original[] = {
      0x8B, 0x41, 0x04,        // mov eax,[ecx+4]
      0x85, 0xC0,              // test eax,eax
      0x75, 0x01, 0xC3,        // jne / ret
      0x8B, 0x08, 0x50,        // mov ecx,[eax] / push eax
      0xFF, 0x51, 0x44,        // call [ecx+44h]
      0x33, 0xD2, 0x85, 0xC0,  // xor edx,edx / test eax,eax
      0x0F, 0x9D, 0xC2,        // setge dl
  };
  constexpr uintptr_t kAt = 0x402B70;
  if (!patch::Matches(kAt, original, sizeof(original))) {
    patch::Log("perf: sound object commit 0x402b70 differs, not changed");
    return;
  }
  patch::Jump(kAt, reinterpret_cast<uintptr_t>(&ObjectCommit), 5);
  patch::Log("perf: sound object commit (0x402b70) also goes to the worker");
}

void PatchSoundCommit() {
  const uint8_t original[] = {
      0xF6, 0x41, 0x2C, 0x01,              // test byte [ecx+2Ch],1
      0x75, 0x03,                          // jne
      0x33, 0xC0, 0xC3,                    // xor eax,eax / ret
      0x8B, 0x81, 0x64, 0x04, 0x00, 0x00,  // mov eax,[ecx+464h]
      0x50, 0x8B, 0x08, 0xFF, 0x51, 0x44,  // push eax / mov ecx,[eax] / call [ecx+44h]
  };
  constexpr uintptr_t kAt = 0x4C32D0;
  if (!patch::Matches(kAt, original, sizeof(original))) {
    patch::Log("perf: 3D sound commit 0x4c32d0 differs, not changed");
    return;
  }
  QueryPerformanceFrequency(&g_commit_freq);
  InitializeCriticalSection(&g_pending_cs);
  g_commit_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE t = g_commit_event ? CreateThread(nullptr, 0, &CommitThread, nullptr, 0, nullptr) : nullptr;
  if (!t) {
    patch::Log("perf: sound commit thread could not be started");
    return;
  }
  CloseHandle(t);
  // thiscall (ecx) -> our __fastcall (ecx): same registers, no stack arguments.
  g_at_present = GetConfig().async_sound >= 2;
  patch::Jump(kAt, reinterpret_cast<uintptr_t>(&SoundCommit), 5);
  PatchObjectCommit();
  patch::Log("perf: 3D sound listener commit (0x4c32d0) runs on a worker thread (AsyncSound=%d%s)",
             GetConfig().async_sound, g_at_present ? ", started at Present" : "");
}

}  // namespace

// Called by both renderers just before Present: the game thread is about to
// wait for VSync, so the pending 3D sound commit runs now.
void PerfBeforePresent() {
  if (g_at_present && g_npending) SetEvent(g_commit_event);
}

// FastGameCode: game loops the 1999 compiler built badly, rewritten in place
// with the same x87 maths (results bit for bit the same).
//
// Terrain patch draw 0x481c00, loop 0x481cc7: unpacks each vertex's two
// 16-bit texture coordinates (dword at +0xc) into floats at +0x10/+0x14,
// times the scale in st(0). The compiler converted each through two dword
// stores and a qword fild, which stalls the CPU (store forwarding fails):
// ~12 ns a vertex, the hottest instruction in a race profile. Now each half
// goes through one dword store and a dword fild: ~1 ns a vertex. ebx stays 0
// as before (the original zeroed it in the loop); the 16 bytes at esp+0x10
// are the function's scratch.
void PatchTerrainUv() {
  static const uint8_t orig[] = {
      0x8D, 0x4F, 0x0C, 0x8B, 0xD6, 0x8B, 0x01, 0x83, 0xC1, 0x20, 0x8B, 0xD8, 0x25, 0xFF, 0xFF,
      0x00, 0x00, 0xC1, 0xEB, 0x10, 0x89, 0x5C, 0x24, 0x10, 0x33, 0xDB, 0x89, 0x5C, 0x24, 0x14,
      0x89, 0x44, 0x24, 0x18, 0xDF, 0x6C, 0x24, 0x10, 0x89, 0x5C, 0x24, 0x1C, 0x4A, 0xD8, 0xC9,
      0xD9, 0x59, 0xE4, 0xDF, 0x6C, 0x24, 0x18, 0xD8, 0xC9, 0xD9, 0x59, 0xE8, 0x75, 0xCA};
  static const uint8_t code[] = {
      0x8D, 0x4F, 0x0C,              // lea ecx,[edi+0Ch]
      0x8B, 0xD6,                    // mov edx,esi
      0x0F, 0xB7, 0x41, 0x02,        // loop: movzx eax,word [ecx+2]
      0x89, 0x44, 0x24, 0x10,        // mov [esp+10h],eax
      0x0F, 0xB7, 0x01,              // movzx eax,word [ecx]
      0x89, 0x44, 0x24, 0x14,        // mov [esp+14h],eax
      0xDB, 0x44, 0x24, 0x10,        // fild dword [esp+10h]
      0xD8, 0xC9,                    // fmul st,st(1)
      0xD9, 0x59, 0x04,              // fstp dword [ecx+4]
      0xDB, 0x44, 0x24, 0x14,        // fild dword [esp+14h]
      0xD8, 0xC9,                    // fmul st,st(1)
      0xD9, 0x59, 0x08,              // fstp dword [ecx+8]
      0x83, 0xC1, 0x20,              // add ecx,20h
      0x4A,                          // dec edx
      0x75, 0xD9,                    // jnz loop
      0xEB, 0x0D,                    // jmp 481cfd
      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
  static_assert(sizeof(orig) == sizeof(code), "same length");
  if (patch::Replace("perf: terrain texture coordinate loop", 0x481CC2, orig, sizeof(orig), code, sizeof(code)))
    patch::Log("perf: FastGameCode=1, terrain texture coordinate loop at 481cc2 rewritten (same results, no store stalls)");
}

void ApplyPerfFixes() {
  if (GetConfig().async_sound) PatchSoundCommit();
  else patch::Log("perf: AsyncSound=0, 3D sound committed on the game thread");
  if (GetConfig().fast_game_code) PatchTerrainUv();
  else patch::Log("perf: FastGameCode=0, game loops left as they are");
  if (!GetConfig().fast_crt) {
    patch::Log("perf: FastCrt=0, game clock and _stricmp left as they are");
    return;
  }
  PatchClock();
  PatchStricmp();
}
