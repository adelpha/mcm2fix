// Input timing: give each race simulation step the input of its own moment.
//
// With SimRate the race runs in fixed 1/30 s steps and a frame runs as many
// steps as real time calls for. The game reads its controls once per step
// (ControlInterface update 0x43dea0, called first in each step):
//   keyboard 0x4c8d30: buffered events (GetDeviceData) -> key down/up state
//     (+0x260 per key), then the keyboard axes (0x48b050) ramp from that state;
//   joystick 0x4c80b0: Poll, then GetDeviceState (immediate, DIJOYSTATE, 80
//     bytes) or, with BufferedJoystick, GetDeviceData like the keyboard.
// That works while every frame runs at most one step. After a long frame
// (a hitch, a dropped frame) the next frame runs two or more steps back to
// back, and:
//   - the first step reads every keyboard event of the whole stall, the others
//     read nothing, so a key pressed late in the stall counts as held for all
//     the catch-up steps, and a key pressed and released within the stall
//     counts for none of them;
//   - a key or button pressed and released between two reads (a quick tap,
//     shorter than one step) is lost: the step sees it go down and up at once,
//     and the steering/throttle axes only look at whether the key is down;
//   - the joystick is read back to back, so every catch-up step gets the stick
//     where it is now, not where it was during that step.
// So the same hands on the controls give different riding depending on
// frame times.
//
// With InputTiming=1 mcm2fix sits between the game and DirectInput:
//   - buffered events (keyboard, buffered joystick) are kept in a queue and
//     each step gets only the events stamped up to the end of that step's
//     slice of real time; the frame's last step gets the rest, so nothing is
//     later than before;
//   - during races a key (or button) released in the same step it was pressed
//     has its release held back to the next step, so every tap is seen for at
//     least one step;
//   - joysticks read immediately are also sampled every 4 ms on a thread;
//     a catch-up step gets the sample from the end of its slice, and buttons
//     that were down in any sample since the last read count as down.
// Outside races everything goes through as before (apart from the keyboard
// read being limited to the 15 events the game's buffer holds).
#include "input_timing.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "config.h"
#include "patch.h"

namespace {

// DirectInput definitions (no dinput.h dependency).
struct DiData {  // DIDEVICEOBJECTDATA (DirectInput 7, 16 bytes)
  DWORD ofs, data, time, seq;
};
constexpr DWORD kPeek = 1;  // DIGDD_PEEK
constexpr HRESULT kBufferOverflow = 1;  // DI_BUFFEROVERFLOW
const GUID kIID_DI7A = {0x9A4CB684, 0x236D, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kIID_DI7W = {0x9A4CB685, 0x236D, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kIID_Dev2A = {0x5944E682, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kIID_Dev2W = {0x5944E683, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kIID_Dev7A = {0x57D7C6BC, 0x2356, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kIID_Dev7W = {0x57D7C6BD, 0x2356, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};

// IDirectInput7 and IDirectInputDevice7 vtable slots.
constexpr int kSlotCreateDevice = 3, kSlotCreateDeviceEx = 9;
constexpr int kSlotRelease = 2, kSlotGetState = 9, kSlotGetData = 10, kSlotSetFormat = 11, kSlotPoll = 25;

constexpr DWORD kKeyboardMax = 15;  // the game's keyboard event buffer (0x4c8d30) holds 15 events
constexpr DWORD kJoyButtons = 0x30;  // DIJOYSTATE: buttons from byte 48
constexpr int kHistory = 512;        // joystick samples kept (about 2 s at 4 ms)
constexpr DWORD kPollMs = 4;

enum Kind { kKeyboard, kMouse, kJoystick };
const char* KindName(int k) { return k == kKeyboard ? "keyboard" : k == kMouse ? "mouse" : "joystick"; }

struct Sample {
  LONGLONG t;
  HRESULT hr;
  uint8_t s[272];
};

struct Device {
  void* iface = nullptr;
  int kind = kJoystick;
  int number = 0;
  bool has_poll = false;
  DWORD format_size = 0;  // SetDataFormat dwDataSize
  std::deque<DiData> queue;          // buffered events not yet given to the game
  std::map<DWORD, unsigned> down_step;  // object -> step id its last press was given in
  // Immediate joystick reads.
  DWORD state_size = 0;      // size the game reads with
  LONGLONG last_read = 0;    // QPC of the game's last read (thread samples while recent)
  LONGLONG served_to = 0;    // samples up to this time are accounted for
  std::vector<Sample> hist;  // ring
  int hist_next = 0, hist_count = 0;
  LONGLONG last_sample = 0;
};

struct Step {
  bool in_step = false, final = true, racing = false;
  int index = 0, count = 1;
  unsigned id = 1;
  double behind = 0.0;
  DWORD cut_ms = 0;
  LONGLONG cut_qpc = 0;
};

struct Stats {
  unsigned frames = 0, catchup_frames = 0, max_steps = 0, steps = 0;
  unsigned events = 0, early_events = 0, held_back = 0, taps = 0, capped = 0, overflows = 0;
  double age_sum = 0.0;  // ms from event stamp to the read that gave it out
  unsigned age_n = 0, bad_stamps = 0;
  unsigned samples = 0, sample_fail = 0, hist_reads = 0, live_reads = 0, or_buttons = 0;
  double gap_sum = 0.0, gap_max = 0.0;
};

using CreateExFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, void*);
using CreateDeviceFn = HRESULT(WINAPI*)(void*, REFGUID, void**, void*);
using CreateDeviceExFn = HRESULT(WINAPI*)(void*, REFGUID, REFIID, void**, void*);
using ReleaseFn = ULONG(WINAPI*)(void*);
using GetStateFn = HRESULT(WINAPI*)(void*, DWORD, void*);
using GetDataFn = HRESULT(WINAPI*)(void*, DWORD, DiData*, DWORD*, DWORD);
using SetFormatFn = HRESULT(WINAPI*)(void*, const void*);
using PollFn = HRESULT(WINAPI*)(void*);

CRITICAL_SECTION g_cs;
CreateExFn g_create = nullptr;
std::map<void**, std::vector<void*>> g_orig;  // vtable -> original slots
std::map<void*, Device*> g_devices;
int g_next_number = 0;
Step g_step;
Stats g_stats;
LARGE_INTEGER g_freq{};
bool g_log = false;
int g_log_left = 3000;
DWORD g_last_summary = 0;
HANDLE g_thread = nullptr;

struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
};

LONGLONG Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double Ms(LONGLONG qpc) { return static_cast<double>(qpc) * 1000.0 / g_freq.QuadPart; }

bool Detail() { return g_log && g_log_left > 0 && g_log_left--; }

void* Orig(void* obj, int slot) {
  void** vt = *reinterpret_cast<void***>(obj);
  auto it = g_orig.find(vt);
  if (it == g_orig.end() || !it->second[slot]) return vt[slot];
  return it->second[slot];
}

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

Device* Find(void* iface) {
  auto it = g_devices.find(iface);
  return it == g_devices.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------- summary

void MaybeSummary() {
  if (!g_log) return;
  const DWORD now = GetTickCount();
  if (g_last_summary == 0) g_last_summary = now;
  if (now - g_last_summary < 10000) return;
  g_last_summary = now;
  Stats& s = g_stats;
  if (!s.frames && !s.events && !s.samples) return;
  patch::Log("input: 10 s: frames=%u catch-up frames=%u (most steps in a frame %u) steps=%u | "
             "events=%u given early in a catch-up=%u held for a later step=%u taps held=%u "
             "capped=%u overflows=%u avg age=%.1f ms bad stamps=%u | joystick samples=%u "
             "failed=%u gap avg=%.2f max=%.2f ms, reads from history=%u live=%u buttons kept=%u",
             s.frames, s.catchup_frames, s.max_steps, s.steps, s.events, s.early_events,
             s.held_back, s.taps, s.capped, s.overflows, s.age_n ? s.age_sum / s.age_n : 0.0,
             s.bad_stamps, s.samples, s.sample_fail, s.samples ? s.gap_sum / s.samples : 0.0,
             s.gap_max, s.hist_reads, s.live_reads, s.or_buttons);
  s = Stats{};
}

// ---------------------------------------------------------------- buffered events

// Stamp usable? DirectInput stamps are GetTickCount milliseconds.
bool StampOk(DWORD stamp, DWORD now_ms) {
  const int age = static_cast<int>(now_ms - stamp);
  return age > -100 && age < 5000;
}

bool IsButton(const Device& d, DWORD ofs) {
  if (d.kind == kKeyboard) return true;
  return d.kind == kJoystick && ofs >= kJoyButtons && ofs < kJoyButtons + 128;
}

// Read everything DirectInput has into the queue.
HRESULT Drain(Device& d, void* self) {
  auto get = reinterpret_cast<GetDataFn>(Orig(self, kSlotGetData));
  HRESULT result = S_OK;
  for (int guard = 0; guard < 64; ++guard) {
    DiData buf[64];
    DWORD n = 64;
    HRESULT hr = get(self, sizeof(DiData), buf, &n, 0);
    if (FAILED(hr)) return hr;
    if (hr == kBufferOverflow) {
      result = hr;
      ++g_stats.overflows;
      if (Detail()) patch::Log("input: %s %d: DirectInput buffer overflowed, events lost", KindName(d.kind), d.number);
    }
    for (DWORD i = 0; i < n; ++i) d.queue.push_back(buf[i]);
    if (n < 64) break;
  }
  return result;
}

// Which queue entries this read may have. reason[i]: kTake, or why not.
enum Reason : uint8_t { kTake, kLater, kTapHeld, kBehindHeld, kLimit };

void Select(Device& d, DWORD limit, std::vector<uint8_t>* reason, DWORD* taken) {
  const Step& st = g_step;
  const bool timed = st.in_step && !st.final && st.racing;
  const bool taps = st.in_step && st.racing;
  const DWORD now_ms = GetTickCount();
  reason->assign(d.queue.size(), kLimit);
  *taken = 0;
  std::vector<DWORD> blocked;  // objects with an event held back: keep their order
  auto is_blocked = [&](DWORD ofs) {
    for (DWORD b : blocked)
      if (b == ofs) return true;
    return false;
  };
  for (size_t i = 0; i < d.queue.size(); ++i) {
    const DiData& e = d.queue[i];
    if (timed && StampOk(e.time, now_ms) && static_cast<int>(e.time - st.cut_ms) > 0) {
      for (size_t j = i; j < d.queue.size(); ++j) (*reason)[j] = kLater;  // stamps are in order
      break;
    }
    if (*taken >= limit) break;
    if (is_blocked(e.ofs)) {
      (*reason)[i] = kBehindHeld;
      continue;
    }
    if (taps && IsButton(d, e.ofs) && !(e.data & 0x80)) {
      auto it = d.down_step.find(e.ofs);
      bool pressed_now = it != d.down_step.end() && it->second == st.id;
      // Pressed earlier in this same read? Then it went down this step too.
      for (size_t j = 0; j < i && !pressed_now; ++j)
        if ((*reason)[j] == kTake && d.queue[j].ofs == e.ofs && (d.queue[j].data & 0x80)) pressed_now = true;
      if (pressed_now) {
        blocked.push_back(e.ofs);
        (*reason)[i] = kTapHeld;
        continue;
      }
    }
    (*reason)[i] = kTake;
    ++*taken;
  }
}

void NoteGiven(Device& d, const DiData& e) {
  const DWORD now_ms = GetTickCount();
  ++g_stats.events;
  if (!g_step.final && g_step.in_step) ++g_stats.early_events;
  if (StampOk(e.time, now_ms)) {
    g_stats.age_sum += static_cast<int>(now_ms - e.time);
    ++g_stats.age_n;
  } else {
    ++g_stats.bad_stamps;
  }
  if (IsButton(d, e.ofs) && (e.data & 0x80)) d.down_step[e.ofs] = g_step.id;
  if (Detail())
    patch::Log("input: %s %d: step %d/%d (#%u, %s) gives ofs=0x%02lx %s=%lu stamp=%lu age=%d ms cut=%d ms ago",
               KindName(d.kind), d.number, g_step.index + 1, g_step.count, g_step.id,
               g_step.in_step ? (g_step.racing ? "race" : "no race") : "outside steps",
               e.ofs, IsButton(d, e.ofs) ? ((e.data & 0x80) ? "down" : "up") : "value", e.data,
               e.time, static_cast<int>(now_ms - e.time),
               (g_step.in_step && !g_step.final) ? static_cast<int>(now_ms - g_step.cut_ms) : 0);
}

HRESULT WINAPI MyGetData(void* self, DWORD cb, DiData* data, DWORD* inout, DWORD flags) {
  Lock lock;
  Device* d = Find(self);
  auto get = reinterpret_cast<GetDataFn>(Orig(self, kSlotGetData));
  if (!d || d->kind == kMouse || cb != sizeof(DiData) || !inout) return get(self, cb, data, inout, flags);
  if (!data && !(flags & kPeek)) {  // flush
    if (Detail()) patch::Log("input: %s %d: flush (%u queued dropped)", KindName(d->kind), d->number, static_cast<unsigned>(d->queue.size()));
    d->queue.clear();
    return get(self, cb, data, inout, flags);
  }
  HRESULT hr = Drain(*d, self);
  if (FAILED(hr)) return hr;  // lost/not acquired: the game re-acquires and reads again; queue kept
  DWORD limit = *inout;
  if (d->kind == kKeyboard && limit > kKeyboardMax) {
    if (!data) limit = kKeyboardMax;  // the peek's count becomes the size of the game's next read
  }
  std::vector<uint8_t> reason;
  DWORD taken = 0;
  Select(*d, limit, &reason, &taken);
  if (d->kind == kKeyboard && !data && taken == kKeyboardMax && d->queue.size() > taken) ++g_stats.capped;
  if (data) {
    DWORD k = 0;
    for (size_t i = 0; i < d->queue.size(); ++i)
      if (reason[i] == kTake) data[k++] = d->queue[i];
    if (!(flags & kPeek)) {
      unsigned later = 0;
      std::deque<DiData> rest;
      for (size_t i = 0; i < d->queue.size(); ++i) {
        const DiData& e = d->queue[i];
        if (reason[i] == kTake) {
          NoteGiven(*d, e);
          continue;
        }
        rest.push_back(e);
        if (reason[i] == kLater) ++later;
        if (reason[i] == kTapHeld) {
          ++g_stats.taps;
          if (Detail())
            patch::Log("input: %s %d: step %d/%d (#%u): ofs=0x%02lx released in the step it was pressed: "
                       "release held for the next step (tap)",
                       KindName(d->kind), d->number, g_step.index + 1, g_step.count, g_step.id, e.ofs);
        }
      }
      g_stats.held_back += later;
      d->queue.swap(rest);
      if (later && Detail())
        patch::Log("input: %s %d: step %d/%d (#%u): %u events after this step's end (%.1f ms ago) wait for a later step",
                   KindName(d->kind), d->number, g_step.index + 1, g_step.count, g_step.id, later,
                   g_step.behind * 1000.0);
    }
  }
  *inout = taken;
  MaybeSummary();
  return hr;
}

// ---------------------------------------------------------------- immediate joystick reads

void PushSample(Device& d, LONGLONG t, HRESULT hr, const void* s) {
  if (d.hist.empty()) d.hist.resize(kHistory);
  Sample& x = d.hist[d.hist_next];
  x.t = t;
  x.hr = hr;
  std::memcpy(x.s, s, d.state_size);
  d.hist_next = (d.hist_next + 1) % kHistory;
  if (d.hist_count < kHistory) ++d.hist_count;
}

// Sample i-th newest (0 = newest).
const Sample& Back(const Device& d, int i) { return d.hist[(d.hist_next - 1 - i + 2 * kHistory) % kHistory]; }

// OR in buttons that were down in any sample in (from, to].
unsigned OrButtons(const Device& d, LONGLONG from, LONGLONG to, uint8_t* state) {
  if (d.state_size < kJoyButtons + 32) return 0;
  const DWORD nb = d.state_size >= kJoyButtons + 128 + 96 ? 128 : 32;  // DIJOYSTATE2: 128 buttons
  unsigned added = 0;
  for (int i = 0; i < d.hist_count; ++i) {
    const Sample& x = Back(d, i);
    if (x.t <= from) break;
    if (x.t > to || FAILED(x.hr)) continue;
    for (DWORD b = 0; b < nb; ++b) {
      if ((x.s[kJoyButtons + b] & 0x80) && !(state[kJoyButtons + b] & 0x80)) {
        state[kJoyButtons + b] = 0x80;
        ++added;
      }
    }
  }
  return added;
}

HRESULT WINAPI MyGetState(void* self, DWORD cb, void* out) {
  Lock lock;
  Device* d = Find(self);
  auto get = reinterpret_cast<GetStateFn>(Orig(self, kSlotGetState));
  if (!d || d->kind != kJoystick || !out || cb > sizeof(Sample::s)) return get(self, cb, out);
  const LONGLONG now = Now();
  if (d->state_size != cb) {
    patch::Log("input: joystick %d: game reads state of %lu bytes (DIJOYSTATE%s)", d->number, cb,
               cb == 80 ? "" : cb == 272 ? "2" : " ?");
    d->state_size = cb;
    d->hist_count = 0;
    d->served_to = 0;
  }
  d->last_read = now;
  const bool use = g_step.in_step && g_step.racing && g_thread;
  if (use && !g_step.final) {
    // The newest good sample from before this step's end.
    for (int i = 0; i < d->hist_count; ++i) {
      const Sample& x = Back(*d, i);
      if (x.t > g_step.cut_qpc || FAILED(x.hr)) continue;
      if (x.t <= d->served_to) break;  // nothing new for this step: read live
      std::memcpy(out, x.s, cb);
      unsigned added = OrButtons(*d, d->served_to, x.t, static_cast<uint8_t*>(out));
      ++g_stats.hist_reads;
      g_stats.or_buttons += added;
      if (Detail())
        patch::Log("input: joystick %d: step %d/%d (#%u) uses sample from %.1f ms ago (step end %.1f ms ago), "
                   "x=%ld y=%ld, %u tapped buttons kept",
                   d->number, g_step.index + 1, g_step.count, g_step.id, Ms(now - x.t), g_step.behind * 1000.0,
                   *reinterpret_cast<const LONG*>(x.s), *reinterpret_cast<const LONG*>(x.s + 4), added);
      d->served_to = x.t;
      MaybeSummary();
      return S_OK;
    }
  }
  HRESULT hr = get(self, cb, out);
  if (SUCCEEDED(hr) && use) {
    unsigned added = OrButtons(*d, d->served_to, now, static_cast<uint8_t*>(out));
    g_stats.or_buttons += added;
    ++g_stats.live_reads;
    if (added && Detail())
      patch::Log("input: joystick %d: step %d/%d (#%u) live read, %u buttons tapped since the last read kept down",
                 d->number, g_step.index + 1, g_step.count, g_step.id, added);
    d->served_to = now;
  }
  MaybeSummary();
  return hr;
}

DWORD WINAPI PollThread(void*) {
  std::vector<uint8_t> buf(sizeof(Sample::s));
  for (;;) {
    Sleep(kPollMs);
    Lock lock;
    if (!g_step.racing) continue;
    const LONGLONG now = Now();
    for (auto& kv : g_devices) {
      Device& d = *kv.second;
      if (kv.first != d.iface || d.kind != kJoystick || !d.state_size) continue;
      if (now - d.last_read > g_freq.QuadPart) continue;  // the game isn't reading it
      if (d.has_poll) reinterpret_cast<PollFn>(Orig(d.iface, kSlotPoll))(d.iface);
      HRESULT hr = reinterpret_cast<GetStateFn>(Orig(d.iface, kSlotGetState))(d.iface, d.state_size, buf.data());
      const LONGLONG t = Now();
      if (FAILED(hr)) ++g_stats.sample_fail;
      if (d.last_sample) {
        double gap = Ms(t - d.last_sample);
        if (gap < 1000.0) {
          g_stats.gap_sum += gap;
          if (gap > g_stats.gap_max) g_stats.gap_max = gap;
        }
      }
      d.last_sample = t;
      ++g_stats.samples;
      PushSample(d, t, hr, buf.data());
    }
  }
  return 0;
}

// ---------------------------------------------------------------- device lifetime

ULONG WINAPI MyRelease(void* self) {
  Lock lock;
  ULONG r = reinterpret_cast<ReleaseFn>(Orig(self, kSlotRelease))(self);
  if (r == 0) {
    if (Device* d = Find(self)) {
      patch::Log("input: %s %d released", KindName(d->kind), d->number);
      g_devices.erase(self);
      delete d;
    }
  }
  return r;
}

HRESULT WINAPI MySetFormat(void* self, const void* fmt) {
  Lock lock;
  HRESULT hr = reinterpret_cast<SetFormatFn>(Orig(self, kSlotSetFormat))(self, fmt);
  if (Device* d = Find(self)) {
    // DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf
    d->format_size = fmt ? reinterpret_cast<const DWORD*>(fmt)[3] : 0;
    patch::Log("input: %s %d: data format %lu bytes, %lu objects (hr=0x%08lx)", KindName(d->kind), d->number,
               d->format_size, fmt ? reinterpret_cast<const DWORD*>(fmt)[4] : 0, hr);
  }
  return hr;
}

void Register(void* dev, REFGUID guid, const GUID* iid) {
  Lock lock;
  if (Find(dev)) return;
  auto* d = new Device;
  d->iface = dev;
  d->kind = IsEqualGUID(guid, kSysKeyboard) ? kKeyboard : IsEqualGUID(guid, kSysMouse) ? kMouse : kJoystick;
  d->number = g_next_number++;
  d->has_poll = iid && (IsEqualGUID(*iid, kIID_Dev2A) || IsEqualGUID(*iid, kIID_Dev2W) ||
                        IsEqualGUID(*iid, kIID_Dev7A) || IsEqualGUID(*iid, kIID_Dev7W));
  g_devices[dev] = d;
  Hook(dev, kSlotRelease, reinterpret_cast<void*>(&MyRelease));
  Hook(dev, kSlotGetState, reinterpret_cast<void*>(&MyGetState));
  Hook(dev, kSlotGetData, reinterpret_cast<void*>(&MyGetData));
  Hook(dev, kSlotSetFormat, reinterpret_cast<void*>(&MySetFormat));
  patch::Log("input: %s %d created (%p, vtable %p, %s)", KindName(d->kind), d->number, dev,
             *reinterpret_cast<void**>(dev), d->has_poll ? "Device2+" : "Device");
  if (d->kind == kJoystick && !g_thread && d->has_poll) {
    g_thread = CreateThread(nullptr, 0, &PollThread, nullptr, 0, nullptr);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    patch::Log("input: joystick sampling thread %s (every %lu ms during races)", g_thread ? "started" : "FAILED", kPollMs);
  }
}

HRESULT WINAPI MyCreateDevice(void* self, REFGUID guid, void** out, void* outer) {
  HRESULT hr = reinterpret_cast<CreateDeviceFn>(Orig(self, kSlotCreateDevice))(self, guid, out, outer);
  if (SUCCEEDED(hr) && out && *out) Register(*out, guid, nullptr);
  return hr;
}

HRESULT WINAPI MyCreateDeviceEx(void* self, REFGUID guid, REFIID iid, void** out, void* outer) {
  HRESULT hr = reinterpret_cast<CreateDeviceExFn>(Orig(self, kSlotCreateDeviceEx))(self, guid, iid, out, outer);
  if (SUCCEEDED(hr) && out && *out) Register(*out, guid, &iid);
  return hr;
}

HRESULT WINAPI MyDirectInputCreateEx(HINSTANCE inst, DWORD version, REFIID iid, void** out, void* outer) {
  HRESULT hr = g_create(inst, version, iid, out, outer);
  const bool v7 = IsEqualGUID(iid, kIID_DI7A) || IsEqualGUID(iid, kIID_DI7W);
  patch::Log("input: DirectInputCreateEx(version 0x%lx, %s) hr=0x%08lx", version, v7 ? "IDirectInput7" : "other interface", hr);
  if (SUCCEEDED(hr) && out && *out) {
    Lock lock;
    Hook(*out, kSlotCreateDevice, reinterpret_cast<void*>(&MyCreateDevice));
    if (v7) Hook(*out, kSlotCreateDeviceEx, reinterpret_cast<void*>(&MyCreateDeviceEx));
  }
  return hr;
}

bool g_enabled = false;

}  // namespace

namespace input {

void Apply() {
  const Config& c = GetConfig();
  if (!c.input_timing) {
    patch::Log("input: InputTiming=0, DirectInput left as it is");
    return;
  }
  QueryPerformanceFrequency(&g_freq);
  InitializeCriticalSection(&g_cs);
  g_log = c.input_log;
  constexpr uintptr_t kDirectInputCreateEx = 0x555028;  // DINPUT!DirectInputCreateEx
  if (!*reinterpret_cast<void**>(kDirectInputCreateEx)) {
    patch::Log("input: DirectInputCreateEx import not bound, InputTiming not installed");
    return;
  }
  g_create = reinterpret_cast<CreateExFn>(
      patch::HookIat(kDirectInputCreateEx, reinterpret_cast<void*>(&MyDirectInputCreateEx)));
  g_enabled = true;
  patch::Log("input: InputTiming=1 (InputLog=%d): race steps get the input of their own time slice, taps held one step",
             g_log ? 1 : 0);
}

void BeginStep(LONGLONG now, double behind, int index, int count, bool racing) {
  if (!g_enabled) return;
  Lock lock;
  Step& s = g_step;
  s.in_step = true;
  s.final = index + 1 >= count;
  s.racing = racing;
  s.index = index;
  s.count = count;
  ++s.id;
  s.behind = behind;
  const LONGLONG back = static_cast<LONGLONG>(behind * g_freq.QuadPart);
  s.cut_qpc = now - back;
  // DirectInput stamps are in GetTickCount milliseconds; measure from now.
  const double since_tick_start = Ms(Now() - now);
  s.cut_ms = GetTickCount() - static_cast<DWORD>(behind * 1000.0 + since_tick_start + 0.5);
  if (racing) {
    ++g_stats.steps;
    if (index == 0) {
      ++g_stats.frames;
      if (count > 1) ++g_stats.catchup_frames;
      if (static_cast<unsigned>(count) > g_stats.max_steps) g_stats.max_steps = count;
      if (count > 1 && Detail()) patch::Log("input: catch-up frame: %d steps, first step ends %.1f ms ago", count, behind * 1000.0);
    }
  }
}

void EndSteps() {
  if (!g_enabled) return;
  Lock lock;
  g_step.in_step = false;
  g_step.final = true;
  ++g_step.id;
}

}  // namespace input
