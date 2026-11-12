// Mute 3D sounds beyond their maximum distance.
//
// The game creates its 3D sound buffers (0x4c1ce0) with
// DSBCAPS_MUTE3DATMAXDISTANCE: Windows' DirectSound silences such a buffer
// while it is further from the listener than its max distance, and the game
// counts on that (it stops feeding sounds that are out of range). DSOAL, the
// DirectSound-on-OpenAL replacement people drop next to the exe for EAX,
// ignores the flag (it only appears in its flag-name list), so those buffers
// keep playing quietly: looped bits of old engine and crash sounds that
// repeat as riders move out of range.
//
// mcm2fix does the muting itself. DirectSoundCreate (the game's import) is
// wrapped and the COM vtables of the device, its buffers, their
// IDirectSound3DBuffer interfaces and the listener are hooked, so we see each
// buffer's position, max distance and mode and the listener position. A
// buffer with the flag that is out of range gets volume DSBVOLUME_MIN; when it
// comes back the game's own volume is restored. Volume changes the game makes
// while a buffer is muted are kept and applied on unmute.
//
// Objects are tracked by the references the game holds (creation, QI, AddRef,
// Release through any interface of the buffer), so we never touch a buffer
// the game has let go of.
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "config.h"
#include "patch.h"

void LogCompat(const char* when);

namespace {

// DirectSound definitions (no dsound.h dependency).
constexpr DWORD kCapsPrimary = 0x1, kCapsCtrl3D = 0x10, kCapsCtrlVolume = 0x80, kCapsMuteMax = 0x20000;
constexpr LONG kVolumeMin = -10000;
constexpr DWORD kModeHeadRelative = 1, kModeDisable = 2;
const GUID kIID_3DBuffer = {0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const GUID kIID_Listener = {0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

struct BufferDesc {
  DWORD size, flags, bytes, reserved;
  void* format;
  GUID alg;
};
struct Vec {
  float x, y, z;
};
struct Ds3dBuffer {  // DS3DBUFFER
  DWORD size;
  Vec pos, vel;
  DWORD in_cone, out_cone;
  Vec orient;
  LONG out_volume;
  float min_dist, max_dist;
  DWORD mode;
};
struct Ds3dListener {  // DS3DLISTENER
  DWORD size;
  Vec pos, vel, front, top;
  float distance_factor, rolloff, doppler;
};

struct Buffer {
  int id = 0;
  DWORD flags = 0;
  DWORD bytes = 0;
  LONG refs = 1;         // references the game holds, over all interfaces
  void* buffer = nullptr;  // the IDirectSoundBuffer
  Vec pos{};
  float max_dist = 1e9f;  // DS3D_DEFAULTMAXDISTANCE
  float min_dist = 1.0f;  // DS3D_DEFAULTMINDISTANCE
  bool logged_min = false;
  DWORD mode = 0;
  LONG volume = 0;        // what the game asked for
  bool muted = false;
  bool playing = false, looping = false;
  bool logged_max = false;
  void* iface3d = nullptr;  // its IDirectSound3DBuffer (SoundPauseLog snapshots)
  unsigned mutes = 0, unmutes = 0;
};

// Originals per vtable, by slot.
std::map<void**, std::vector<void*>> g_orig;
std::map<void*, Buffer*> g_iface;  // any interface pointer of a tracked buffer -> record
std::vector<Buffer*> g_buffers;
void* g_listener = nullptr;
Vec g_listener_pos{};
bool g_listener_set = false;  // no muting until the game has placed the listener
float g_rolloff = 1.0f, g_distance_factor = 1.0f;
bool g_dirty = false;         // deferred 3D changes waiting for CommitDeferredSettings
volatile LONG g_deferred = 0, g_immediate = 0, g_commit_checks = 0;
constexpr DWORD kDeferred = 1;  // DS3D_DEFERRED
constexpr float kUnmuteFraction = 0.98f;  // back in range only below 98% of max distance
CRITICAL_SECTION g_cs;
int g_next_id = 0;
int g_next_any = 0;  // 2D and primary buffers, numbered from 1000 in the logs
bool g_log = false;
bool g_watch = false;  // SoundMaxDistanceMute=2: track and log out-of-range sounds, change nothing
volatile LONG g_restarts = 0, g_restarts_out = 0, g_plays = 0;
bool g_cs_ready = false;
int g_log_left = 200;  // detailed lines per run
volatile LONG g_mutes = 0, g_unmutes = 0, g_plays_out = 0, g_vol_held = 0;
DWORD g_last_summary = 0;

// SoundPauseLog: every buffer the game creates (2D, 3D and the primary), so
// their state can be logged when a race is paused and unpaused.
struct AnyBuf {
  int id = 0;
  DWORD flags = 0, bytes = 0;
};
std::map<void*, AnyBuf> g_all;
bool g_pause_log = false;
int g_mode = 1;  // 0 = no muting, 1 = mute, 2 = watch only (SoundMaxDistanceMute, 1 settled at DirectSoundCreate)
void* g_device = nullptr;
HWND g_coop_hwnd = nullptr;
DWORD g_coop_level = 0;
int g_fail_logs = 60;  // failed DirectSound calls logged per run
volatile LONG g_fails = 0;

using CreateFn = HRESULT(WINAPI*)(const GUID*, void**, void*);
CreateFn g_create = nullptr;

void* Orig(void* obj, int slot) {
  void** vt = *reinterpret_cast<void***>(obj);
  auto it = g_orig.find(vt);
  if (it == g_orig.end() || !it->second[slot]) return vt[slot];
  return it->second[slot];
}

// Points vtable slot of obj's class at fn (once per vtable and slot).
void Hook(void* obj, int slot, void* fn) {
  void** vt = *reinterpret_cast<void***>(obj);
  auto& o = g_orig[vt];
  if (o.empty()) o.resize(32);
  if (o[slot] || vt[slot] == fn) return;
  o[slot] = vt[slot];
  DWORD old;
  if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) {
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
  }
}

const char* g_trace_tag = nullptr;  // sound trace running (RestartLog, SoundPauseLog)
bool Detail() { return (g_log || g_pause_log) && g_log_left > 0 && g_log_left--; }
volatile LONG g_all_commits = 0, g_set3d_deferred = 0, g_set3d_now = 0;

Buffer* Find(void* iface) {
  auto it = g_iface.find(iface);
  return it == g_iface.end() ? nullptr : it->second;
}

using SetVolumeFn = HRESULT(WINAPI*)(void*, LONG);

// Number of any buffer (SoundPauseLog), -1 if unknown. Called under g_cs.
int AnyId(void* iface) {
  if (Buffer* b = Find(iface)) return b->id;
  auto it = g_all.find(iface);
  return it == g_all.end() ? -1 : it->second.id;
}

// A DirectSound call that failed: the first few are logged with the buffer
// and the game code that made the call. Called under g_cs.
void Failed(void* self, const char* op, HRESULT hr) {
  InterlockedIncrement(&g_fails);
  if (g_fail_logs <= 0) return;
  --g_fail_logs;
  volatile uintptr_t marker = 0;
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(const_cast<uintptr_t*>(&marker));
  uintptr_t chain[3] = {};
  int n = 0;
  for (int i = 0; i < 96 && n < 3; ++i) {
    if (IsBadReadPtr(sp + i, 4)) break;
    if (sp[i] >= 0x401000 && sp[i] < 0x553000) chain[n++] = sp[i];
  }
  patch::Log("sound: %s on buffer #%d %p failed %08lx (via %08x %08x %08x)%s", op, AnyId(self), self,
             static_cast<unsigned long>(hr), static_cast<unsigned>(chain[0]), static_cast<unsigned>(chain[1]),
             static_cast<unsigned>(chain[2]), hr == static_cast<HRESULT>(0x88780096) ? " - buffer lost" : "");
}

float Distance(const Buffer& b) {
  Vec d = b.pos;
  if (b.mode != kModeHeadRelative) {
    d.x -= g_listener_pos.x;
    d.y -= g_listener_pos.y;
    d.z -= g_listener_pos.z;
  }
  return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

// Mute or unmute b to match its distance. Called under g_cs.
void Update(Buffer& b, const char* why) {
  if (g_mode == 0 || !(b.flags & kCapsMuteMax) || !b.buffer || !g_listener_set) return;
  const float dist = Distance(b);
  // A muted sound must come a little closer than max distance to be heard
  // again, so one sitting right on the edge doesn't flip every frame.
  const float limit = b.muted ? b.max_dist * kUnmuteFraction : b.max_dist;
  const bool out = b.mode != kModeDisable && dist > limit;
  if (out == b.muted) return;
  b.muted = out;  // in watch mode: would be muted
  const HRESULT hr =
      g_watch ? S_OK : reinterpret_cast<SetVolumeFn>(Orig(b.buffer, 0x3c / 4))(b.buffer, out ? kVolumeMin : b.volume);
  InterlockedIncrement(out ? &g_mutes : &g_unmutes);
  ++(out ? b.mutes : b.unmutes);
  if (Detail()) {
    // DirectSound's attenuation stops at max distance at this level, then the
    // flag mutes: gain = min / (min + rolloff * (max - min)).
    const float m = b.min_dist > 0 ? b.min_dist : 1.0f;
    const float g = m / (m + g_rolloff * (b.max_dist > m ? b.max_dist - m : 0.0f));
    patch::Log("sound: buffer #%d %s (%s): distance %.1f, min %.1f, max %.1f, rolloff %.2f (level at max %.1f dB), "
               "mode %lu, %s%s, game volume %ld, hr %08lx",
               b.id, g_watch ? (out ? "out of range (watch only)" : "back in range (watch only)") : (out ? "muted" : "unmuted"), why, dist, b.min_dist, b.max_dist, g_rolloff,
               20.0f * std::log10(g > 1e-6f ? g : 1e-6f), b.mode, b.playing ? "playing" : "stopped",
               b.looping ? " looping" : "", b.volume, static_cast<unsigned long>(hr));
  }
}

// Windows' DirectSound applies deferred 3D settings, and so the mute, at
// CommitDeferredSettings. Checking at each call instead sees the listener
// moved but a sound not yet (or the other way round), which flipped sounds
// near the edge every frame (perf-18).
void Changed(Buffer& b, DWORD apply, const char* why) {
  InterlockedIncrement((apply & kDeferred) ? &g_set3d_deferred : &g_set3d_now);
  if (apply & kDeferred) {
    g_dirty = true;
    InterlockedIncrement(&g_deferred);
  } else {
    InterlockedIncrement(&g_immediate);
    Update(b, why);
  }
}

void Summary() {
  const DWORD now = GetTickCount();
  if (!g_log || now - g_last_summary < 10000) return;
  g_last_summary = now;
  int muted = 0, playing = 0, playing_muted = 0;
  for (Buffer* b : g_buffers) {
    muted += b->muted;
    playing += b->playing;
    playing_muted += b->playing && b->muted;
  }
  patch::Log("sound: 10s: %d 3D buffers tracked, %d muted now, %d playing (%d of them muted); %ld mutes, %ld unmutes, "
             "%ld plays (%ld started out of range; %ld of all plays hit a buffer still playing, %ld of those out of range), "
             "%ld volume changes held while muted; listener at %.1f %.1f %.1f; "
             "3D changes %ld deferred / %ld immediate, %ld commits checked",
             static_cast<int>(g_buffers.size()), muted, playing, playing_muted, InterlockedExchange(&g_mutes, 0),
             InterlockedExchange(&g_unmutes, 0), InterlockedExchange(&g_plays, 0), InterlockedExchange(&g_plays_out, 0),
             InterlockedExchange(&g_restarts, 0),
             InterlockedExchange(&g_restarts_out, 0),
             InterlockedExchange(&g_vol_held, 0), g_listener_pos.x, g_listener_pos.y, g_listener_pos.z,
             InterlockedExchange(&g_deferred, 0), InterlockedExchange(&g_immediate, 0),
             InterlockedExchange(&g_commit_checks, 0));
}

void Forget(Buffer* b) {
  for (auto it = g_iface.begin(); it != g_iface.end();)
    it = it->second == b ? g_iface.erase(it) : std::next(it);
  for (size_t i = 0; i < g_buffers.size(); ++i)
    if (g_buffers[i] == b) {
      g_buffers.erase(g_buffers.begin() + i);
      break;
    }
  if (g_log && b->id < 64) patch::Log("sound: buffer #%d released by the game", b->id);
  g_all.erase(b->buffer);
  delete b;
}

struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
};

// ------------------------------------------------- IUnknown on tracked objects

void HookUnknown(void* obj);
void HookBuffer(void* obj);
void Hook3D(void* obj);
void HookListener(void* obj);

HRESULT WINAPI QueryInterface(void* self, const GUID& iid, void** out) {
  using Fn = HRESULT(WINAPI*)(void*, const GUID&, void**);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0))(self, iid, out);
  if (FAILED(hr) || !out || !*out) return hr;
  Lock lock;
  if (iid == kIID_Listener) {
    if (g_listener != *out) {
      g_listener = *out;
      HookListener(*out);
      patch::Log("sound: 3D listener %p", *out);
    }
    return hr;
  }
  Buffer* b = Find(self);
  if (!b) return hr;
  ++b->refs;
  g_iface[*out] = b;
  if (iid == kIID_3DBuffer) {
    Hook3D(*out);
    b->iface3d = *out;
  }
  else HookUnknown(*out);
  return hr;
}

ULONG WINAPI AddRef(void* self) {
  using Fn = ULONG(WINAPI*)(void*);
  const ULONG n = reinterpret_cast<Fn>(Orig(self, 1))(self);
  Lock lock;
  if (Buffer* b = Find(self)) ++b->refs;
  return n;
}

ULONG WINAPI Release(void* self) {
  using Fn = ULONG(WINAPI*)(void*);
  Lock lock;  // held across the call so the record goes with the object
  Buffer* b = Find(self);
  void* orig = Orig(self, 2);
  const ULONG n = reinterpret_cast<Fn>(orig)(self);
  if (b && --b->refs <= 0) Forget(b);
  if (self == g_listener && n == 0) g_listener = nullptr;
  if (n == 0 && self == g_device) {
    g_device = nullptr;
    g_all.clear();
  }
  if (n == 0 && g_all.erase(self) && g_pause_log && g_fail_logs > 0 && g_all.size() < 4)
    patch::Log("sound: buffer %p released, %u buffers left", self, static_cast<unsigned>(g_all.size()));
  return n;
}

void HookUnknown(void* obj) {
  Hook(obj, 0, reinterpret_cast<void*>(&QueryInterface));
  Hook(obj, 1, reinterpret_cast<void*>(&AddRef));
  Hook(obj, 2, reinterpret_cast<void*>(&Release));
}

// ------------------------------------------------------- IDirectSoundBuffer

HRESULT WINAPI GetVolume(void* self, LONG* v) {
  using Fn = HRESULT(WINAPI*)(void*, LONG*);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x18 / 4))(self, v);
  Lock lock;
  Buffer* b = Find(self);
  if (b && b->muted && !g_watch && v && SUCCEEDED(hr)) *v = b->volume;
  return hr;
}


// ---- RestartLog sound trace ----
// For a few seconds after a race load and after Restart Race, every
// SetVolume / SetFrequency / Play / Stop on a buffer is summarised per
// (buffer, call, game call chain), so the engine sound the game picks during
// the countdown can be compared between a fresh start and a restart.
struct TraceKey {
  int id;
  char op;
  uintptr_t chain[4];
  bool operator<(const TraceKey& o) const {
    if (id != o.id) return id < o.id;
    if (op != o.op) return op < o.op;
    return std::memcmp(chain, o.chain, sizeof(chain)) < 0;
  }
};
struct TraceVal {
  unsigned count = 0, fails = 0;
  long first = 0, lo = 0, hi = 0, last = 0;
  DWORD bytes = 0;
};
std::map<TraceKey, TraceVal> g_trace;
DWORD g_trace_end = 0;

void TraceFlush() {
  if (!g_trace_tag) return;
  patch::Log("sound trace (%s): %u call sites", g_trace_tag, static_cast<unsigned>(g_trace.size()));
  for (const auto& t : g_trace)
    patch::Log("sound trace (%s): #%d %u bytes %c x%u (%u failed) first %ld min %ld max %ld last %ld via %08x %08x %08x %08x",
               g_trace_tag, t.first.id, static_cast<unsigned>(t.second.bytes), t.first.op, t.second.count, t.second.fails,
               t.second.first, t.second.lo, t.second.hi, t.second.last, static_cast<unsigned>(t.first.chain[0]),
               static_cast<unsigned>(t.first.chain[1]), static_cast<unsigned>(t.first.chain[2]),
               static_cast<unsigned>(t.first.chain[3]));
  g_trace.clear();
  g_trace_tag = nullptr;
}

// Called under g_cs.
__declspec(noinline) void Trace(void* self, char op, long v, HRESULT hr = S_OK) {
  if (!g_trace_tag) return;
  if (static_cast<LONG>(GetTickCount() - g_trace_end) >= 0) {
    TraceFlush();
    return;
  }
  Buffer* b = Find(self);
  TraceKey k{};
  k.id = AnyId(self);
  k.op = op;
  // Game code addresses on the stack above us: the call site and its callers.
  volatile uintptr_t marker = 0;
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(const_cast<uintptr_t*>(&marker));
  int n = 0;
  for (int i = 0; i < 96 && n < 4; ++i) {
    uintptr_t a = 0;
    if (IsBadReadPtr(sp + i, 4)) break;
    a = sp[i];
    if (a >= 0x401000 && a < 0x553000) k.chain[n++] = a;
  }
  TraceVal& t = g_trace[k];
  if (!t.count) t.first = t.lo = t.hi = v;
  ++t.count;
  if (v < t.lo) t.lo = v;
  if (v > t.hi) t.hi = v;
  t.last = v;
  if (FAILED(hr)) ++t.fails;
  auto it = g_all.find(self);
  t.bytes = b ? b->bytes : it != g_all.end() ? it->second.bytes : 0;
}

HRESULT WINAPI SetFrequency(void* self, DWORD f) {
  using Fn = HRESULT(WINAPI*)(void*, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x44 / 4))(self, f);
  Lock lock;
  Trace(self, 'F', static_cast<long>(f), hr);
  if (FAILED(hr)) Failed(self, "SetFrequency", hr);
  return hr;
}

HRESULT WINAPI SetVolume(void* self, LONG v) {
  Lock lock;
  Buffer* b = Find(self);
  if (b) {
    b->volume = v;
    if (b->muted && !g_watch) {
      Trace(self, 'V', v);
      InterlockedIncrement(&g_vol_held);
      return S_OK;
    }
  }
  const HRESULT hr = reinterpret_cast<SetVolumeFn>(Orig(self, 0x3c / 4))(self, v);
  Trace(self, 'V', v, hr);
  if (FAILED(hr)) Failed(self, "SetVolume", hr);
  return hr;
}

HRESULT WINAPI Play(void* self, DWORD r1, DWORD prio, DWORD flags) {
  using Fn = HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD);
  {
    Lock lock;
    if (Buffer* b = Find(self)) {
      // Started again while still sounding: the game restarts the sample
      // from wherever its play cursor is (DirectSound doesn't rewind on Play).
      using StatusFn = HRESULT(WINAPI*)(void*, DWORD*);
      DWORD st = 0;
      if (SUCCEEDED(reinterpret_cast<StatusFn>(Orig(self, 0x24 / 4))(self, &st)) && (st & 1)) {
        InterlockedIncrement(&g_restarts);
        if (b->muted) InterlockedIncrement(&g_restarts_out);
      }
      b->playing = true;
      InterlockedIncrement(&g_plays);
      b->looping = flags & 1;  // DSBPLAY_LOOPING
      Update(*b, "play");
      if (b->muted) InterlockedIncrement(&g_plays_out);
      Summary();
    }
  }
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x30 / 4))(self, r1, prio, flags);
  Lock lock;
  Trace(self, 'P', static_cast<long>(flags), hr);
  if (FAILED(hr)) Failed(self, "Play", hr);
  return hr;
}

HRESULT WINAPI Stop(void* self) {
  using Fn = HRESULT(WINAPI*)(void*);
  {
    Lock lock;
    if (Buffer* b = Find(self)) b->playing = false;
  }
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x48 / 4))(self);
  Lock lock;
  Trace(self, 'S', 0, hr);
  if (FAILED(hr)) Failed(self, "Stop", hr);
  return hr;
}

// SoundPauseLog only: the remaining calls that can start, stop or lose sound.
HRESULT WINAPI SetCurrentPosition(void* self, DWORD pos) {
  using Fn = HRESULT(WINAPI*)(void*, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x34 / 4))(self, pos);
  Lock lock;
  Trace(self, 'C', static_cast<long>(pos), hr);
  if (FAILED(hr)) Failed(self, "SetCurrentPosition", hr);
  return hr;
}

HRESULT WINAPI Restore(void* self) {
  using Fn = HRESULT(WINAPI*)(void*);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x50 / 4))(self);
  Lock lock;
  Trace(self, 'R', 0, hr);
  if (g_fail_logs > 0) patch::Log("sound: Restore on buffer #%d %p: %08lx", AnyId(self), self, static_cast<unsigned long>(hr));
  return hr;
}

volatile LONG g_lost_seen = 0;
HRESULT WINAPI GetStatus(void* self, DWORD* st) {
  using Fn = HRESULT(WINAPI*)(void*, DWORD*);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x24 / 4))(self, st);
  if (SUCCEEDED(hr) && st && (*st & 2)) {  // DSBSTATUS_BUFFERLOST
    Lock lock;
    if (InterlockedIncrement(&g_lost_seen) <= 20)
      patch::Log("sound: game's GetStatus on buffer #%d %p says the buffer is lost (status %08lx)", AnyId(self), self,
                 static_cast<unsigned long>(*st));
  } else if (FAILED(hr)) {
    Lock lock;
    Failed(self, "GetStatus", hr);
  }
  return hr;
}

void HookBuffer(void* obj) {
  HookUnknown(obj);
  Hook(obj, 0x18 / 4, reinterpret_cast<void*>(&GetVolume));
  Hook(obj, 0x30 / 4, reinterpret_cast<void*>(&Play));
  Hook(obj, 0x3c / 4, reinterpret_cast<void*>(&SetVolume));
  if (GetConfig().restart_log || g_pause_log) Hook(obj, 0x44 / 4, reinterpret_cast<void*>(&SetFrequency));
  Hook(obj, 0x48 / 4, reinterpret_cast<void*>(&Stop));
  if (g_pause_log) {
    Hook(obj, 0x24 / 4, reinterpret_cast<void*>(&GetStatus));
    Hook(obj, 0x34 / 4, reinterpret_cast<void*>(&SetCurrentPosition));
    Hook(obj, 0x50 / 4, reinterpret_cast<void*>(&Restore));
  }
}

// ----------------------------------------------------- IDirectSound3DBuffer

HRESULT WINAPI SetAllParameters3D(void* self, const Ds3dBuffer* p, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, const Ds3dBuffer*, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x30 / 4))(self, p, apply);
  Lock lock;
  Trace(self, 'A', static_cast<long>(apply), hr);
  Buffer* b = Find(self);
  if (b && p && SUCCEEDED(hr)) {
    b->pos = p->pos;
    b->max_dist = p->max_dist;
    b->min_dist = p->min_dist;
    b->mode = p->mode;
    Changed(*b, apply, "all parameters");
  }
  return hr;
}

HRESULT WINAPI SetMaxDistance(void* self, float d, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, float, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x40 / 4))(self, d, apply);
  Lock lock;
  Buffer* b = Find(self);
  if (b && SUCCEEDED(hr)) {
    if (!b->logged_max && g_log && b->id < 64) {
      b->logged_max = true;
      patch::Log("sound: buffer #%d max distance %.1f", b->id, d);
    }
    b->max_dist = d;
    Changed(*b, apply, "max distance");
  }
  return hr;
}

HRESULT WINAPI SetMinDistance(void* self, float d, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, float, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x44 / 4))(self, d, apply);
  Lock lock;
  Buffer* b = Find(self);
  if (b && SUCCEEDED(hr)) {
    if (!b->logged_min && g_log && b->id < 64) {
      b->logged_min = true;
      patch::Log("sound: buffer #%d min distance %.1f", b->id, d);
    }
    b->min_dist = d;
  }
  return hr;
}

HRESULT WINAPI SetMode(void* self, DWORD mode, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, DWORD, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x48 / 4))(self, mode, apply);
  Lock lock;
  Trace(self, 'M', static_cast<long>(mode), hr);
  Buffer* b = Find(self);
  if (b && SUCCEEDED(hr)) {
    b->mode = mode;
    Changed(*b, apply, "mode");
  }
  return hr;
}

HRESULT WINAPI SetPosition3D(void* self, float x, float y, float z, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, float, float, float, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x4c / 4))(self, x, y, z, apply);
  Lock lock;
  Trace(self, 'X', static_cast<long>(apply), hr);
  Buffer* b = Find(self);
  if (b && SUCCEEDED(hr)) {
    b->pos = {x, y, z};
    Changed(*b, apply, "position");
  }
  return hr;
}

void Hook3D(void* obj) {
  HookUnknown(obj);
  Hook(obj, 0x30 / 4, reinterpret_cast<void*>(&SetAllParameters3D));
  Hook(obj, 0x40 / 4, reinterpret_cast<void*>(&SetMaxDistance));
  Hook(obj, 0x44 / 4, reinterpret_cast<void*>(&SetMinDistance));
  Hook(obj, 0x48 / 4, reinterpret_cast<void*>(&SetMode));
  Hook(obj, 0x4c / 4, reinterpret_cast<void*>(&SetPosition3D));
}

// ---------------------------------------------------- IDirectSound3DListener

void ListenerMoved(const Vec& p, DWORD apply) {
  if (!g_listener_set) patch::Log("sound: listener first placed at %.1f %.1f %.1f, muting starts", p.x, p.y, p.z);
  g_listener_set = true;
  g_listener_pos = p;
  if (apply & kDeferred) {
    g_dirty = true;
    InterlockedIncrement(&g_deferred);
    return;
  }
  InterlockedIncrement(&g_immediate);
  for (Buffer* b : g_buffers) Update(*b, "listener moved");
  Summary();
}

// Runs where the commit runs (the AsyncSound worker or the game thread).
HRESULT WINAPI ListenerCommit(void* self) {
  using Fn = HRESULT(WINAPI*)(void*);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x44 / 4))(self);
  Lock lock;
  InterlockedIncrement(&g_all_commits);
  if (FAILED(hr)) Failed(self, "CommitDeferredSettings", hr);
  if (g_dirty) {
    g_dirty = false;
    InterlockedIncrement(&g_commit_checks);
    for (Buffer* b : g_buffers) Update(*b, "commit");
  }
  Summary();
  return hr;
}

HRESULT WINAPI ListenerSetAll(void* self, const Ds3dListener* p, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, const Ds3dListener*, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x28 / 4))(self, p, apply);
  Lock lock;
  if (self == g_listener && p && SUCCEEDED(hr)) {
    if (p->rolloff != g_rolloff || p->distance_factor != g_distance_factor)
      patch::Log("sound: listener rolloff %.2f, distance factor %.4f (all parameters)", p->rolloff, p->distance_factor);
    g_rolloff = p->rolloff;
    g_distance_factor = p->distance_factor;
    ListenerMoved(p->pos, apply);
  }
  return hr;
}

HRESULT WINAPI ListenerSetPosition(void* self, float x, float y, float z, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, float, float, float, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x38 / 4))(self, x, y, z, apply);
  Lock lock;
  if (self == g_listener && SUCCEEDED(hr)) ListenerMoved({x, y, z}, apply);
  return hr;
}

HRESULT WINAPI ListenerSetRolloff(void* self, float f, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, float, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x3c / 4))(self, f, apply);
  Lock lock;
  if (self == g_listener && SUCCEEDED(hr)) {
    if (f != g_rolloff) patch::Log("sound: listener rolloff %.2f", f);
    g_rolloff = f;
  }
  return hr;
}

HRESULT WINAPI ListenerSetDistanceFactor(void* self, float f, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, float, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x2c / 4))(self, f, apply);
  Lock lock;
  if (self == g_listener && SUCCEEDED(hr)) {
    if (f != g_distance_factor) patch::Log("sound: listener distance factor %.4f", f);
    g_distance_factor = f;
  }
  return hr;
}

void HookListener(void* obj) {
  Hook(obj, 0x2c / 4, reinterpret_cast<void*>(&ListenerSetDistanceFactor));
  Hook(obj, 0x3c / 4, reinterpret_cast<void*>(&ListenerSetRolloff));
  Hook(obj, 0x28 / 4, reinterpret_cast<void*>(&ListenerSetAll));
  Hook(obj, 0x38 / 4, reinterpret_cast<void*>(&ListenerSetPosition));
  Hook(obj, 0x44 / 4, reinterpret_cast<void*>(&ListenerCommit));
}

// ------------------------------------------------------------- IDirectSound

Buffer* Track(void* buf, DWORD flags, DWORD bytes) {
  Buffer* b = new Buffer;
  b->id = g_next_id++;
  b->flags = flags;
  b->bytes = bytes;
  b->buffer = buf;
  using Fn = HRESULT(WINAPI*)(void*, LONG*);
  LONG v = 0;
  if (SUCCEEDED(reinterpret_cast<Fn>(Orig(buf, 0x18 / 4))(buf, &v))) b->volume = v;
  g_iface[buf] = b;
  g_buffers.push_back(b);
  HookBuffer(buf);
  return b;
}

HRESULT WINAPI CreateSoundBuffer(void* self, BufferDesc* desc, void** out, void* outer) {
  using Fn = HRESULT(WINAPI*)(void*, BufferDesc*, void**, void*);
  DWORD flags = desc ? desc->flags : 0;
  const bool track = (flags & kCapsCtrl3D) && !(flags & kCapsPrimary);
  BufferDesc copy;
  if (track && g_mode == 1 && (flags & kCapsMuteMax) && !(flags & kCapsCtrlVolume)) {
    copy = *desc;  // we need volume control to mute it
    copy.flags |= kCapsCtrlVolume;
    desc = &copy;
  }
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0xc / 4))(self, desc, out, outer);
  if (FAILED(hr) || !out || !*out) {
    if (g_log && track) patch::Log("sound: 3D buffer creation (flags %08lx) failed %08lx", desc ? desc->flags : 0, static_cast<unsigned long>(hr));
    if (g_pause_log) patch::Log("sound: CreateSoundBuffer (flags %08lx) failed %08lx", flags, static_cast<unsigned long>(hr));
    return hr;
  }
  Lock lock;
  if (g_pause_log) {
    AnyBuf& a = g_all[*out];
    a.id = track ? g_next_id : 1000 + g_next_any++;  // 3D buffers keep their 3D number
    a.flags = desc->flags;
    a.bytes = desc->bytes;
    if (flags & kCapsPrimary)
      patch::Log("sound: primary buffer %p created (flags %08lx)", *out, a.flags);
  }
  if (flags & kCapsPrimary) {
    HookBuffer(*out);  // the listener comes from its QueryInterface
    return hr;
  }
  if (!track) {
    if (g_pause_log) HookBuffer(*out);  // 2D sounds: only watched
    return hr;
  }
  Buffer* b = Track(*out, desc->flags, desc->bytes);
  if (g_log && b->id < 64)
    patch::Log("sound: 3D buffer #%d %p: flags %08lx%s, %lu bytes, volume %ld", b->id, *out, b->flags,
               flags != b->flags ? " (volume control added)" : "", b->bytes, b->volume);
  return hr;
}

HRESULT WINAPI DuplicateSoundBuffer(void* self, void* src, void** out) {
  using Fn = HRESULT(WINAPI*)(void*, void*, void**);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x14 / 4))(self, src, out);
  if (FAILED(hr) || !out || !*out) return hr;
  Lock lock;
  if (g_pause_log) {
    auto it = g_all.find(src);
    AnyBuf a;
    a.flags = it != g_all.end() ? it->second.flags : 0;
    a.bytes = it != g_all.end() ? it->second.bytes : 0;
    a.id = Find(src) ? g_next_id : 1000 + g_next_any++;
    g_all[*out] = a;
    if (!Find(src)) HookBuffer(*out);
  }
  if (Buffer* s = Find(src)) {
    Buffer* b = Track(*out, s->flags, s->bytes);
    b->max_dist = s->max_dist;
    b->min_dist = s->min_dist;
    b->mode = s->mode;
    b->pos = s->pos;
    if (g_log && b->id < 64) patch::Log("sound: 3D buffer #%d %p duplicated from #%d", b->id, *out, s->id);
  }
  return hr;
}

HRESULT WINAPI SetCooperativeLevel(void* self, HWND hwnd, DWORD level) {
  using Fn = HRESULT(WINAPI*)(void*, HWND, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x18 / 4))(self, hwnd, level);
  Lock lock;
  if (SUCCEEDED(hr)) {
    g_coop_hwnd = hwnd;
    g_coop_level = level;
  }
  char cls[64] = {};
  if (hwnd) GetClassNameA(hwnd, cls, sizeof(cls));
  patch::Log("sound: SetCooperativeLevel(%p \"%s\", level %lu) -> %08lx", static_cast<void*>(hwnd), cls, level,
             static_cast<unsigned long>(hr));
  return hr;
}

// True when path is Windows' own dsound.dll (System32, or SysWOW64 as a
// 32-bit process may report it), not a replacement next to the exe.
bool IsWindowsDsound(const char* path) {
  const char* slash = std::strrchr(path, '\\');
  if (!slash) return false;
  const size_t dir_len = static_cast<size_t>(slash - path);
  char sys[MAX_PATH] = {};
  UINT n = GetSystemDirectoryA(sys, MAX_PATH);
  if (n && n == dir_len && _strnicmp(sys, path, dir_len) == 0) return true;
  using Wow64DirFn = UINT(WINAPI*)(LPSTR, UINT);
  if (auto fn = reinterpret_cast<Wow64DirFn>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemWow64DirectoryA")))) {
    n = fn(sys, MAX_PATH);
    if (n && n == dir_len && _strnicmp(sys, path, dir_len) == 0) return true;
  }
  return false;
}

HRESULT WINAPI MyDirectSoundCreate(const GUID* dev, void** out, void* outer) {
  const HRESULT hr = g_create(dev, out, outer);
  if (SUCCEEDED(hr) && out && *out) {
    Lock lock;
    g_device = *out;
    Hook(*out, 0xc / 4, reinterpret_cast<void*>(&CreateSoundBuffer));
    Hook(*out, 0x14 / 4, reinterpret_cast<void*>(&DuplicateSoundBuffer));
    Hook(*out, 0x18 / 4, reinterpret_cast<void*>(&SetCooperativeLevel));
    if (g_pause_log) Hook(*out, 2, reinterpret_cast<void*>(&Release));  // device gone: forget its buffers
    HMODULE m = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(**reinterpret_cast<void***>(*out)), &m))
      GetModuleFileNameA(m, path, MAX_PATH);
    LogCompat("DirectSoundCreate");
    // Windows' own DirectSound already mutes these buffers at max distance,
    // and our volume changes on top of its own confuse it: after a sound we
    // had muted once is stopped and played again (every pause), Windows
    // keeps it silent until its volume is set again (StOo, 2026-11-12: the
    // bike engines went quiet after each pause until they had left and
    // re-entered range). So SoundMaxDistanceMute=1 only acts when the
    // DirectSound is a replacement (DSOAL); 3 forces it.
    const int setting = GetConfig().sound_max_distance_mute;
    if (setting == 1 && path[0] && IsWindowsDsound(path)) {
      g_mode = 0;
      g_watch = true;
      patch::Log("sound: Windows' own DirectSound, it mutes 3D sounds at max distance itself; "
                 "SoundMaxDistanceMute=1 leaves them to it (3 = mute anyway)");
    }
    patch::Log("sound: DirectSoundCreate -> %p (%s); 3D buffers beyond their max distance will be %s", *out, path,
               g_mode == 0 ? "left to DirectSound" : g_watch ? "logged only (watch mode)" : "muted");
  } else {
    patch::Log("sound: DirectSoundCreate failed %08lx", static_cast<unsigned long>(hr));
  }
  return hr;
}

}  // namespace

// Which exe this is and whether Windows' compatibility layer is active in the
// process: an AppCompat layer or shim matched by exe name can change what the
// game's DirectSound calls do (Mcm2.exe vs a renamed copy behave differently).
void LogCompat(const char* when) {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  char layer[512] = {};
  const DWORD n = GetEnvironmentVariableA("__COMPAT_LAYER", layer, sizeof(layer));
  char mods[256] = {};
  const char* names[] = {"apphelp.dll", "AcLayers.dll", "AcGenral.dll", "AcSpecfc.dll", "AcXtrnal.dll", "AcWinRT.dll",
                         "aclua.dll"};
  for (const char* m : names)
    if (GetModuleHandleA(m)) {
      if (mods[0]) strncat(mods, ", ", sizeof(mods) - strlen(mods) - 1);
      strncat(mods, m, sizeof(mods) - strlen(mods) - 1);
    }
  patch::Log("compat (%s): exe %s; __COMPAT_LAYER %s; compatibility modules loaded: %s", when, exe,
             n && n < sizeof(layer) ? layer : "(not set)", mods[0] ? mods : "none");
}

void SoundTraceStart(const char* tag, DWORD ms) {
  if (!g_cs_ready) return;
  Lock lock;
  TraceFlush();
  g_trace_tag = tag;
  g_trace_end = GetTickCount() + ms;
}

void ApplySoundFixes() {
  LogCompat("start");
  g_mode = GetConfig().sound_max_distance_mute;
  if (g_mode == 3) g_mode = 1;  // always, whatever DirectSound it is
  if (g_mode < 0 || g_mode > 3) g_mode = 1;
  g_pause_log = GetConfig().sound_pause_log;
  if (g_pause_log) g_log_left = 600;
  if (!g_mode && !g_pause_log) {
    patch::Log("sound: SoundMaxDistanceMute=0, 3D sounds left to DirectSound");
    return;
  }
  InitializeCriticalSection(&g_cs);
  g_cs_ready = true;
  g_watch = g_mode != 1;
  g_log = GetConfig().sound_log || g_mode == 2;
  constexpr uintptr_t kDirectSoundCreate = 0x55503c;  // DSOUND ordinal 1
  void* prev = *reinterpret_cast<void**>(kDirectSoundCreate);
  if (!prev) {
    patch::Log("sound: DirectSoundCreate import not bound, max-distance mute not installed");
    return;
  }
  g_create = reinterpret_cast<CreateFn>(patch::HookIat(kDirectSoundCreate, reinterpret_cast<void*>(&MyDirectSoundCreate)));
  patch::Log("sound: DirectSoundCreate wrapped (SoundMaxDistanceMute=%d, SoundLog=%d, SoundPauseLog=%d)",
             GetConfig().sound_max_distance_mute, g_log ? 1 : 0, g_pause_log ? 1 : 0);
}

// ---------------------------------------------------------- SoundPauseLog
//
// With Windows' own DirectSound all sound stopped after pausing and
// unpausing a race. Each time the game's pause flag (Game+0x2d5 bit 0)
// changes, every DirectSound buffer call is traced for a few seconds and
// the state of every buffer, the cooperative window and the game's own sound
// switch is logged at the change and a few times after it.
namespace {

int g_pause_events = 0;      // pause changes seen
DWORD g_snap_due[4] = {};    // snapshots still to take (tick counts, 0 = none)
const char* g_snap_tag[4] = {};
bool g_last_paused = false;
bool g_pause_known = false;

const char* WindowInfo(HWND w, char* out, size_t n) {
  char cls[64] = {}, title[64] = {};
  DWORD pid = 0;
  if (w) {
    GetClassNameA(w, cls, sizeof(cls));
    GetWindowTextA(w, title, sizeof(title));
    GetWindowThreadProcessId(w, &pid);
  }
  std::snprintf(out, n, "%p \"%s\" \"%s\" pid %lu%s%s%s", static_cast<void*>(w), cls, title, pid,
            pid == GetCurrentProcessId() ? " (ours)" : "", w && IsIconic(w) ? " minimised" : "",
            w && !IsWindowVisible(w) ? " hidden" : "");
  out[n - 1] = 0;
  return out;
}

// The game's sound switch tested by every sound call: [[0x5743ac]+4]+0x2c
// (the sound object of 0x4c32d0; bit 0 = sound on).
int GameSoundSwitch() {
  const auto* app = reinterpret_cast<const uintptr_t*>(0x5743ac);
  if (IsBadReadPtr(app, 4) || !*app || IsBadReadPtr(reinterpret_cast<void*>(*app + 4), 4)) return -1;
  const uintptr_t snd = *reinterpret_cast<const uintptr_t*>(*app + 4);
  if (!snd || IsBadReadPtr(reinterpret_cast<void*>(snd + 0x2c), 1)) return -1;
  return *reinterpret_cast<const uint8_t*>(snd + 0x2c);
}

void Snapshot(const char* tag) {
  Lock lock;
  char a[256], b[256];
  const HWND fg = GetForegroundWindow();
  const HWND root = g_coop_hwnd ? GetAncestor(g_coop_hwnd, GA_ROOTOWNER) : nullptr;
  patch::Log("sound pause (%s): game sound switch %d (bit 0 = sound on); cooperative window %s level %lu; "
             "foreground %s%s; active %p, focus %p", tag, GameSoundSwitch(), WindowInfo(g_coop_hwnd, a, sizeof(a)),
             g_coop_level, WindowInfo(fg, b, sizeof(b)),
             fg == g_coop_hwnd ? " = cooperative window" : fg == root ? " = its root window" : " - NOT the cooperative window",
             static_cast<void*>(GetActiveWindow()), static_cast<void*>(GetFocus()));
  using StatusFn = HRESULT(WINAPI*)(void*, DWORD*);
  using VolFn = HRESULT(WINAPI*)(void*, LONG*);
  int playing = 0, looping = 0, lost = 0, failed = 0;
  for (const auto& e : g_all) {
    void* buf = e.first;
    const Buffer* t = Find(buf);
    if ((e.second.flags & kCapsCtrl3D) && !(e.second.flags & kCapsPrimary) && !t) continue;  // let go of
    DWORD st = 0;
    LONG vol = 0, freq = 0;
    const HRESULT hs = reinterpret_cast<StatusFn>(Orig(buf, 0x24 / 4))(buf, &st);
    const HRESULT hv = reinterpret_cast<VolFn>(Orig(buf, 0x18 / 4))(buf, &vol);
    const HRESULT hf = reinterpret_cast<VolFn>(Orig(buf, 0x20 / 4))(buf, &freq);
    if (FAILED(hs)) ++failed;
    playing += (st & 1) != 0;
    looping += (st & 4) != 0;
    lost += (st & 2) != 0;
    const bool primary = e.second.flags & kCapsPrimary;
    // Every buffer that is playing, lost or failed, plus the primary; the
    // rest are counted only.
    if (primary || (st & 3) || FAILED(hs) || g_pause_events <= 2 || (t && (t->muted || t->id < 4)))
      patch::Log("sound pause (%s):   #%d %p %s %lu bytes: status %08lx%s%s%s (hr %08lx), volume %ld (hr %08lx)%s, "
                 "frequency %ld (hr %08lx)%s",
                 tag, e.second.id, buf, primary ? "PRIMARY" : (e.second.flags & kCapsCtrl3D) ? "3D" : "2D",
                 e.second.bytes, st, (st & 1) ? " playing" : "", (st & 4) ? " looping" : "", (st & 2) ? " LOST" : "",
                 static_cast<unsigned long>(hs), vol, static_cast<unsigned long>(hv),
                 t ? (t->muted ? " [muted by mcm2fix]" : "") : "", freq, static_cast<unsigned long>(hf),
                 t ? (t->playing ? " [game started it]" : " [game stopped it]") : "");
    if (t && ((st & 3) || t->muted || g_pause_events <= 2 || t->id < 4)) {
      // What DirectSound itself holds for the buffer (deferred values included).
      Vec dp{};
      float dmax = -1;
      DWORD dmode = 99;
      HRESULT hp = E_POINTER;
      if (void* i3 = t->iface3d) {
        using GetVecFn = HRESULT(WINAPI*)(void*, Vec*);
        using GetFloatFn = HRESULT(WINAPI*)(void*, float*);
        using GetDwFn = HRESULT(WINAPI*)(void*, DWORD*);
        hp = reinterpret_cast<GetVecFn>(Orig(i3, 0x28 / 4))(i3, &dp);
        reinterpret_cast<GetFloatFn>(Orig(i3, 0x1c / 4))(i3, &dmax);
        reinterpret_cast<GetDwFn>(Orig(i3, 0x24 / 4))(i3, &dmode);
      }
      patch::Log("sound pause (%s):     3D #%d: our distance %.1f (pos %.1f %.1f %.1f), max %.1f, mode %lu, %s "
                 "(%u mutes, %u unmutes so far), game volume %ld; DirectSound has pos %.1f %.1f %.1f (hr %08lx), "
                 "max %.1f, mode %lu",
                 tag, t->id, Distance(*t), t->pos.x, t->pos.y, t->pos.z, t->max_dist, t->mode,
                 t->muted ? "MUTED by mcm2fix" : "not muted", t->mutes, t->unmutes, t->volume, dp.x, dp.y, dp.z,
                 static_cast<unsigned long>(hp), dmax, dmode);
    }
  }
  Vec lp{};
  HRESULT hl = E_POINTER;
  if (g_listener) hl = reinterpret_cast<HRESULT(WINAPI*)(void*, Vec*)>(Orig(g_listener, 0x1c / 4))(g_listener, &lp);
  patch::Log("sound pause (%s): listener %p at %.1f %.1f %.1f (placed %d; DirectSound has %.1f %.1f %.1f, hr %08lx), "
             "3D changes waiting for a commit %d; since last snapshot: %ld listener commits, %ld deferred / %ld "
             "immediate 3D buffer changes, %ld mutes, %ld unmutes", tag,
             g_listener, g_listener_pos.x, g_listener_pos.y, g_listener_pos.z, g_listener_set ? 1 : 0, lp.x, lp.y, lp.z,
             static_cast<unsigned long>(hl), g_dirty ? 1 : 0, InterlockedExchange(&g_all_commits, 0),
             InterlockedExchange(&g_set3d_deferred, 0), InterlockedExchange(&g_set3d_now, 0),
             InterlockedExchange(&g_mutes, 0), InterlockedExchange(&g_unmutes, 0));
  patch::Log("sound pause (%s): %u buffers, %d playing, %d looping, %d lost, %d status calls failed; %ld failed "
             "DirectSound calls so far",
             tag, static_cast<unsigned>(g_all.size()), playing, looping, lost, failed, g_fails);
}

void PauseChanged(bool paused) {
  ++g_pause_events;
  if (g_pause_events > 16) return;  // race start plus the first pause/unpause pairs
  const char* tag = paused ? "paused" : "unpaused";
  patch::Log("sound pause: race %s (change %d; race simulation stopped or started)", tag, g_pause_events);
  {
    Lock lock;
    TraceFlush();
    g_trace_tag = paused ? "after pause" : "after unpause";
    g_trace_end = GetTickCount() + (paused ? 3000 : 6000);
  }
  Snapshot(tag);
  const DWORD now = GetTickCount();
  for (auto& d : g_snap_due) d = 0;
  if (paused) {
    g_snap_due[0] = now + 1000;
    g_snap_tag[0] = "1 s after pause";
  } else {
    const DWORD at[] = {300, 1500, 4000};
    const char* names[] = {"0.3 s after unpause", "1.5 s after unpause", "4 s after unpause"};
    for (int i = 0; i < 3; ++i) {
      g_snap_due[i] = now + at[i];
      g_snap_tag[i] = names[i];
    }
  }
}

}  // namespace

// Called by the game tick (framerate.cpp) when the race simulation stops or
// starts again (the pause menu stops the bikes).
void SoundRaceRunning(bool running) {
  if (!g_pause_log || !g_cs_ready) return;
  PauseChanged(!running);
}

// Called by the game tick (framerate.cpp) with the Game object.
void SoundPausePoll(uintptr_t game) {
  if (!g_pause_log || !g_cs_ready || !game) return;
  const bool paused = *reinterpret_cast<uint8_t*>(game + 0x2d5) & 1;
  if (!g_pause_known) {
    g_pause_known = true;
    g_last_paused = paused;
  } else if (paused != g_last_paused) {
    g_last_paused = paused;
    PauseChanged(paused);
  }
  const DWORD now = GetTickCount();
  for (int i = 0; i < 4; ++i)
    if (g_snap_due[i] && static_cast<LONG>(now - g_snap_due[i]) >= 0) {
      g_snap_due[i] = 0;
      Snapshot(g_snap_tag[i]);
    }
  if (g_trace_tag && static_cast<LONG>(now - g_trace_end) >= 0) {
    Lock lock;
    TraceFlush();  // also when no sound calls come any more
  }
}
