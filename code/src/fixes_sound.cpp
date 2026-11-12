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
#include <cstring>
#include <map>
#include <vector>

#include "config.h"
#include "patch.h"

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
bool g_log = false;
bool g_watch = false;  // SoundMaxDistanceMute=2: track and log out-of-range sounds, change nothing
volatile LONG g_restarts = 0, g_restarts_out = 0, g_plays = 0;
int g_log_left = 200;  // detailed lines per run
volatile LONG g_mutes = 0, g_unmutes = 0, g_plays_out = 0, g_vol_held = 0;
DWORD g_last_summary = 0;

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

bool Detail() { return g_log && g_log_left > 0 && g_log_left--; }

Buffer* Find(void* iface) {
  auto it = g_iface.find(iface);
  return it == g_iface.end() ? nullptr : it->second;
}

using SetVolumeFn = HRESULT(WINAPI*)(void*, LONG);

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
  if (!(b.flags & kCapsMuteMax) || !b.buffer || !g_listener_set) return;
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
  if (iid == kIID_3DBuffer) Hook3D(*out);
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

HRESULT WINAPI SetVolume(void* self, LONG v) {
  Lock lock;
  Buffer* b = Find(self);
  if (b) {
    b->volume = v;
    if (b->muted && !g_watch) {
      InterlockedIncrement(&g_vol_held);
      return S_OK;
    }
  }
  return reinterpret_cast<SetVolumeFn>(Orig(self, 0x3c / 4))(self, v);
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
  return reinterpret_cast<Fn>(Orig(self, 0x30 / 4))(self, r1, prio, flags);
}

HRESULT WINAPI Stop(void* self) {
  using Fn = HRESULT(WINAPI*)(void*);
  {
    Lock lock;
    if (Buffer* b = Find(self)) b->playing = false;
  }
  return reinterpret_cast<Fn>(Orig(self, 0x48 / 4))(self);
}

void HookBuffer(void* obj) {
  HookUnknown(obj);
  Hook(obj, 0x18 / 4, reinterpret_cast<void*>(&GetVolume));
  Hook(obj, 0x30 / 4, reinterpret_cast<void*>(&Play));
  Hook(obj, 0x3c / 4, reinterpret_cast<void*>(&SetVolume));
  Hook(obj, 0x48 / 4, reinterpret_cast<void*>(&Stop));
}

// ----------------------------------------------------- IDirectSound3DBuffer

HRESULT WINAPI SetAllParameters3D(void* self, const Ds3dBuffer* p, DWORD apply) {
  using Fn = HRESULT(WINAPI*)(void*, const Ds3dBuffer*, DWORD);
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0x30 / 4))(self, p, apply);
  Lock lock;
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
  if (track && !g_watch && (flags & kCapsMuteMax) && !(flags & kCapsCtrlVolume)) {
    copy = *desc;  // we need volume control to mute it
    copy.flags |= kCapsCtrlVolume;
    desc = &copy;
  }
  const HRESULT hr = reinterpret_cast<Fn>(Orig(self, 0xc / 4))(self, desc, out, outer);
  if (FAILED(hr) || !out || !*out) {
    if (g_log && track) patch::Log("sound: 3D buffer creation (flags %08lx) failed %08lx", desc ? desc->flags : 0, static_cast<unsigned long>(hr));
    return hr;
  }
  Lock lock;
  if (flags & kCapsPrimary) {
    HookBuffer(*out);  // the listener comes from its QueryInterface
    return hr;
  }
  if (!track) return hr;
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

HRESULT WINAPI MyDirectSoundCreate(const GUID* dev, void** out, void* outer) {
  const HRESULT hr = g_create(dev, out, outer);
  if (SUCCEEDED(hr) && out && *out) {
    Lock lock;
    Hook(*out, 0xc / 4, reinterpret_cast<void*>(&CreateSoundBuffer));
    Hook(*out, 0x14 / 4, reinterpret_cast<void*>(&DuplicateSoundBuffer));
    HMODULE m = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(**reinterpret_cast<void***>(*out)), &m))
      GetModuleFileNameA(m, path, MAX_PATH);
    patch::Log("sound: DirectSoundCreate -> %p (%s); 3D buffers beyond their max distance will be %s", *out, path,
               g_watch ? "logged only (watch mode)" : "muted");
  } else {
    patch::Log("sound: DirectSoundCreate failed %08lx", static_cast<unsigned long>(hr));
  }
  return hr;
}

}  // namespace

void ApplySoundFixes() {
  if (!GetConfig().sound_max_distance_mute) {
    patch::Log("sound: SoundMaxDistanceMute=0, 3D sounds left to DirectSound");
    return;
  }
  InitializeCriticalSection(&g_cs);
  g_watch = GetConfig().sound_max_distance_mute == 2;
  g_log = GetConfig().sound_log || g_watch;
  constexpr uintptr_t kDirectSoundCreate = 0x55503c;  // DSOUND ordinal 1
  void* prev = *reinterpret_cast<void**>(kDirectSoundCreate);
  if (!prev) {
    patch::Log("sound: DirectSoundCreate import not bound, max-distance mute not installed");
    return;
  }
  g_create = reinterpret_cast<CreateFn>(patch::HookIat(kDirectSoundCreate, reinterpret_cast<void*>(&MyDirectSoundCreate)));
  patch::Log("sound: DirectSoundCreate wrapped (SoundMaxDistanceMute=%d, SoundLog=%d)",
             GetConfig().sound_max_distance_mute, g_log ? 1 : 0);
}
