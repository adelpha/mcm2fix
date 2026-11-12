#include "racetitle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../config.h"
#include "../patch.h"
#include "backend.h"
#include "gdihook.h"
#include "gputext.h"
#include "hudtext.h"
#include "oneshot.h"
#include "surface.h"

// Race title intro (ini RaceTitle=1).
//
// The game shows the event's name during the countdown as small green text
// in the bottom-left corner: the race HUD (built at 0x4e5xxx) pushes it into
// its TextQueueOverlay (HUD object +0x6c) with "add item" 0x520b90, called
// from 0x4e539b. Item: char text[0x80], float seconds (+0x80, 3.25).
//
// Instead, mcm2fix draws it in the middle of the screen with GPU text:
// a white line grows from a dot to the text's width + 30%, the name slides
// up out of the line and the lap count (stunts: time limit) slides down out
// of it, they hold, and the same runs backwards at the same speed so it is
// gone when the countdown ends.
//
// Countdown: BikeRace (the `this` of 0x421f90; [[game+0x558]+0x34] via
// 0x45e300 can be 0 in a race, so it is not used to find it) +0x15c is the time
// left in seconds; 0x421f90 sets it (8.2 with a start gate, 5.2 without, 0
// in modes 0 and 4 and online). The race update (0x41ebb4) counts it down by
// the step time and starts the race (+0x18a = 1) when it runs out. 0x421f90
// is called at race start (0x41ea6f), on Restart Race (0x4215d7) and from
// 0x451ef1.

namespace dd {
namespace {

constexpr uintptr_t kGame = 0x5743AC;
constexpr uintptr_t kQueueAdd = 0x520B90;    // TextQueueOverlay add (thiscall, item*), ret 4
constexpr uintptr_t kNameAddCall = 0x4E539B;  // HUD setup: race name into the queue
constexpr uintptr_t kCountdownInit = 0x421F90;  // BikeRace countdown set (thiscall)
constexpr uintptr_t kCountdownCalls[] = {0x41EA6F, 0x4215D7, 0x451EF1};
constexpr uintptr_t kRaceHolder = 0x558, kHolderRace = 0x34;
constexpr uintptr_t kCurrentQueue = 0x45E350;  // the current event's HUD text queue, or 0 (cdecl, no args)
constexpr uintptr_t kCountdownLeft = 0x15C;  // float seconds
constexpr uintptr_t kRaceStarted = 0x18A;    // byte
constexpr uintptr_t kMode = 0x2D70, kEvent = 0x2D74, kLaps = 0x2D90, kStuntEvent = 0x55C, kBaja = 0x560;
constexpr uintptr_t kTimeLimitMinutes = 0x2EB0;  // float (stunt panel 0x51f460: limit * 60 - elapsed)
constexpr uintptr_t kOnline = 0x3428;
constexpr uintptr_t kDropStart = 0x520840;  // DropTextOverlay start (thiscall)
constexpr uintptr_t kDropStartCalls[] = {0x405C76, 0x50A7B6};
constexpr uintptr_t kDropText = 0x48;
// Podium cutscene (0x45f610 -> 0x45e490 sets it up, then game+0x3430 = 1).
constexpr uintptr_t kPodium = 0x3430, kPodium4 = 0x3434, kPodium8 = 0x3438;
// Every other text queue add, logged with RaceTitleLog (finding the podium's
// "You have finished" text).
constexpr uintptr_t kQueueAddCalls[] = {0x41E2B1, 0x41E717, 0x42080F, 0x420A2C, 0x420B35, 0x4607DD, 0x4608EA,
                                        0x497287, 0x497908, 0x4992D6, 0x4E54B0, 0x4E5AD2, 0x4E5BE3, 0x4E9415};
// Bike update 0x497740 queues the finish text ("Congratulations! You finished
// 1st!", LANG.DLL 0x1481 + place) as the player crosses the line; for the top
// three the podium cutscene starts ~0.15 s later (game+0x3430 = 1) and runs
// while game+0x3434 (game seconds) counts up to 7.0 (constant 0x55582c).
constexpr uintptr_t kFinishAddCall = 0x497B11;
constexpr float kPodiumLength = 7.0f;
constexpr double kPodiumWait = 0.6;  // real seconds to wait for the cutscene before giving the text back
constexpr float kPodiumLineY = 0.16f;  // line height on screen (countdown/final lap: 0.352)

// Timing (seconds).
constexpr float kLineTime = 0.30f;   // dot -> full line
constexpr float kSlideTime = 0.40f;  // text out of the line
constexpr float kNoCountdownHold = 2.0f;  // hold when there is no countdown to fill
constexpr float kFinalLapHold = 2.0f;     // FINAL LAP held open

template <typename T>
T Rd(uintptr_t a) {
  return *reinterpret_cast<const T*>(a);
}

using QueueAddFn = void(__fastcall*)(uintptr_t self, void* edx, const void* item);
using InitFn = void(__fastcall*)(uintptr_t self, void* edx);

bool Logging() { return GetConfig().race_title_log; }

double Now() {
  static LARGE_INTEGER f{};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

struct State {
  std::string name;      // captured race name
  std::string sub;       // "3 Laps" / "3:00 Time Limit"
  uintptr_t race = 0;    // BikeRace whose countdown runs
  uintptr_t vtable = 0;  // its vtable when the countdown was set (sanity check each frame)
  float total = 0;       // countdown length at start
  bool real_time = false;  // no countdown: play on our own clock
  double start_clock = 0;
  bool active = false;
  // Smoothing between simulation steps: the game's countdown moves 30 times a second.
  float last_left = -1;
  double last_change = 0;
  int phase = -1;
  unsigned frames = 0, inits = 0, names = 0, swallowed = 0, no_dc = 0, lost = 0;
  double last_trace = 0;
  bool drawn_this_frame = false;
  // Announcer: countdown voices at whole seconds left, air horn at the start.
  bool snd_active = false;
  float snd_prev = 0;
} g;

const char* const kCountWav[] = {"one.wav", "two.wav", "three.wav", "four.wav", "five.wav"};
constexpr const char* kGoWav = "airhorn.wav";
constexpr const char* kFinalLapWav = "finallap.wav";
constexpr const char* kPodiumWav = "podium.wav";

// FINAL LAP banner.
struct FinalLap {
  std::string text;
  double start = 0;
  bool active = false;
  int phase = -1;
  unsigned frames = 0, starts = 0;
  double last_trace = 0;
} fl;

// Podium finish banner.
struct Podium {
  std::string text;
  uintptr_t queue = 0;
  char item[0x84] = {};  // copy of the queue item, handed back if no cutscene follows
  bool pending = false, active = false;
  double captured = 0, started = 0;
  float last_e = -1;
  double last_change = 0;
  int phase = -1;
  unsigned frames = 0, captures = 0, given_back = 0;
  double last_trace = 0;
} pd;

uintptr_t CurrentRace() {
  const uintptr_t game = Rd<uintptr_t>(kGame);
  if (!game) return 0;
  const uintptr_t holder = Rd<uintptr_t>(game + kRaceHolder);
  return holder ? Rd<uintptr_t>(holder + kHolderRace) : 0;
}

std::string Subtitle(uintptr_t game, char* why, size_t why_size) {
  const int mode = Rd<int>(game + kMode), event = Rd<int>(game + kEvent), laps = Rd<int>(game + kLaps);
  const uintptr_t stunt = Rd<uintptr_t>(game + kStuntEvent);
  const float limit = Rd<float>(game + kTimeLimitMinutes);
  char b[64] = {};
  if (stunt && limit > 0 && limit < 1000) {
    const int secs = static_cast<int>(std::lround(limit * 60.0f));
    std::snprintf(b, sizeof(b), "%d:%02d Time Limit", secs / 60, secs % 60);
    std::snprintf(why, why_size, "stunt event %08x, time limit %.2f min", static_cast<unsigned>(stunt), limit);
  } else if (mode != 0 && mode != 4 && laps > 0 && laps < 1000) {
    std::snprintf(b, sizeof(b), "%d Lap%s", laps, laps == 1 ? "" : "s");
    std::snprintf(why, why_size, "lap count +%x", static_cast<unsigned>(kLaps));
  } else {
    std::snprintf(why, why_size, "no lap count or time limit (mode %d, laps %d, stunt %08x, limit %.2f)", mode, laps,
                  static_cast<unsigned>(stunt), limit);
  }
  (void)event;
  return b;
}

void LogGame(const char* what) {
  const uintptr_t game = Rd<uintptr_t>(kGame);
  if (!game) {
    Log("race title: %s, no game object", what);
    return;
  }
  Log("race title: %s: mode %d event %d laps %d stunt %08x baja %08x limit %.2f min online %d", what,
      Rd<int>(game + kMode), Rd<int>(game + kEvent), Rd<int>(game + kLaps),
      static_cast<unsigned>(Rd<uintptr_t>(game + kStuntEvent)), static_cast<unsigned>(Rd<uintptr_t>(game + kBaja)),
      Rd<float>(game + kTimeLimitMinutes), Rd<int>(game + kOnline));
}

// 0x4e539b: the HUD hands the race name to its text queue.
void __fastcall NameAddHook(uintptr_t queue, void* edx, const char* item) {
  ++g.names;
  char text[0x81] = {};
  if (item) std::memcpy(text, item, 0x80);
  const float secs = item ? Rd<float>(reinterpret_cast<uintptr_t>(item) + 0x80) : 0;
  const bool take = GetConfig().race_title && GpuTextActive() && text[0];
  Log("race title: race name \"%s\" (%.2f s in text queue %08x) %s", text, secs, static_cast<unsigned>(queue),
      take ? "taken: drawn as the title intro, not as corner text"
           : (GetConfig().race_title ? "left to the game (GPU text off or empty)" : "left to the game (RaceTitle=0)"));
  if (take) {
    g.name = text;
    ++g.swallowed;
    return;
  }
  reinterpret_cast<QueueAddFn>(kQueueAdd)(queue, edx, item);
}

void Start(uintptr_t race) {
  ++g.inits;
  const float left = race ? Rd<float>(race + kCountdownLeft) : 0;
  g.race = race;
  g.vtable = race ? Rd<uintptr_t>(race) : 0;
  g.total = left;
  g.real_time = !(left > 0.5f && left < 60.0f);
  if (g.real_time) g.total = kLineTime + kSlideTime + kNoCountdownHold + kLineTime + kSlideTime;
  g.start_clock = Now();
  g.last_left = -1;
  g.last_change = g.start_clock;
  g.phase = -1;
  g.frames = 0;
  g.active = true;  // the name may come later (the HUD can be built after the countdown is set)
  g.snd_active = GetConfig().announcer && !g.real_time;
  g.snd_prev = left;
  if (g.snd_active) {
    for (const char* w : kCountWav) OneShotPreload(w);
    OneShotPreload(kGoWav);
    OneShotPreload(kFinalLapWav);
    OneShotPreload(kPodiumWav);
  }
  const uintptr_t game = Rd<uintptr_t>(kGame);
  char why[160] = {};
  g.sub = game ? Subtitle(game, why, sizeof(why)) : std::string();
  Log("race title: countdown set on race %08x (vtable %08x, game+558 chain %08x): %.2f s%s; title \"%s\", subtitle \"%s\" (%s)%s",
      static_cast<unsigned>(race), static_cast<unsigned>(g.vtable), static_cast<unsigned>(CurrentRace()), left,
      g.real_time ? " - no countdown, playing on the clock" : "", g.name.c_str(), g.sub.c_str(), why,
      g.name.empty() ? " - no name captured yet" : "");
  LogGame("at countdown");
}

void __fastcall CountdownInitHook(uintptr_t race, void* edx) {
  reinterpret_cast<InitFn>(kCountdownInit)(race, edx);
  if (GetConfig().race_title || GetConfig().announcer) Start(race);
}

__attribute__((noinline)) void __fastcall QueueAddLogHook(uintptr_t queue, void* edx, const char* item) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  char text[0x81] = {};
  if (item) std::memcpy(text, item, 0x80);
  Log("race title: text queue %08x add from %08x: \"%s\" (%.2f s)", static_cast<unsigned>(queue),
      static_cast<unsigned>(ret - 5), text, item ? Rd<float>(reinterpret_cast<uintptr_t>(item) + 0x80) : 0.0f);
  reinterpret_cast<QueueAddFn>(kQueueAdd)(queue, edx, item);
}

// 0x497b11: the finish text. Kept back and shown as a banner over the podium
// cutscene; if no cutscene starts (4th or lower), given back to the queue.
void __fastcall FinishAddHook(uintptr_t queue, void* edx, const char* item) {
  char text[0x81] = {};
  if (item) std::memcpy(text, item, 0x80);
  const float secs = item ? Rd<float>(reinterpret_cast<uintptr_t>(item) + 0x80) : 0;
  const bool take = GetConfig().race_title && GpuTextActive() && text[0] && queue;
  ++pd.captures;
  Log("podium: finish text \"%s\" (%.2f s, text queue %08x) %s", text, secs, static_cast<unsigned>(queue),
      take ? "kept back for the podium banner" : "left to the game");
  if (!take) {
    reinterpret_cast<QueueAddFn>(kQueueAdd)(queue, edx, item);
    return;
  }
  if (pd.pending)
    Log("podium: previous finish text \"%s\" was still waiting; replaced", pd.text.c_str());
  pd.text = text;
  pd.queue = queue;
  std::memcpy(pd.item, item, sizeof(pd.item));
  pd.pending = true;
  pd.active = false;
  pd.captured = Now();
}

// Podium cutscene watch (RaceTitleLog): when it starts and ends, and what
// the fields next to the flag do meanwhile.
void PodiumWatch() {
  if (!Logging()) return;
  static int last = -1;
  static double start = 0, last_trace = 0;
  const uintptr_t game = Rd<uintptr_t>(kGame);
  if (!game) return;
  const int on = Rd<int>(game + kPodium);
  const double now = Now();
  const uint32_t a = Rd<uint32_t>(game + kPodium4), b = Rd<uint32_t>(game + kPodium8);
  float fa, fb;
  std::memcpy(&fa, &a, 4);
  std::memcpy(&fb, &b, 4);
  if (on != last) {
    if (on) start = now;
    Log("podium: game+3430 %d -> %d (%.2f s since start); +3434 %08x (%g) +3438 %08x (%g)", last, on,
        on ? 0.0 : now - start, a, fa, b, fb);
    last = on;
  } else if (on && now - last_trace >= 0.5) {
    last_trace = now;
    Log("podium: %.2f s: +3434 %08x (%g) +3438 %08x (%g)", now - start, a, fa, b, fb);
  }
}

// FINAL LAP: the HUD update (0x405c10 / 0x50a750) starts its DropTextOverlay
// (start 0x520840: +0x30 time = 0, +0x34 running = 1; text char[] at +0x48,
// dropped down the screen by update 0x5206c0, drawn with GDI by 0x520740).
// Instead of starting it, show the text as a banner.
void __fastcall DropStartHook(uintptr_t overlay, void* edx) {
  const char* text = overlay ? reinterpret_cast<const char*>(overlay + kDropText) : "";
  char t[0x80] = {};
  std::snprintf(t, sizeof(t), "%s", text);
  const bool take = GetConfig().race_title && GpuTextActive() && t[0];
  ++fl.starts;
  Log("final lap: drop text \"%s\" (overlay %08x) %s", t, static_cast<unsigned>(overlay),
      take ? "shown as a banner instead" : "left to the game");
  if (GetConfig().announcer) {
    // A one-lap race: the final lap is the first, and FINAL LAP shows as it
    // starts, on top of the air horn. Skip the voice then.
    const uintptr_t game = Rd<uintptr_t>(kGame);
    const int laps = game ? Rd<int>(game + kLaps) : 0;
    if (laps == 1)
      Log("announcer: %s skipped, one-lap race (it would play over the start)", kFinalLapWav);
    else
      OneShotPlay(kFinalLapWav, "final lap");
  }
  if (!take) {
    reinterpret_cast<InitFn>(kDropStart)(overlay, edx);
    return;
  }
  fl.text = t;
  fl.start = Now();
  fl.active = true;
  fl.phase = -1;
  fl.frames = 0;
}

bool PatchCall(uintptr_t site, uintptr_t original, const void* hook) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
  if (p[0] != 0xE8 || site + 5 + Rd<int32_t>(site + 1) != original) {
    Log("race title: call at %08x is not to %08x, not hooked", static_cast<unsigned>(site),
        static_cast<unsigned>(original));
    return false;
  }
  patch::Call(site, hook, 5);
  return true;
}

float Clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
float EaseOut(float x) {
  x = Clamp01(x);
  return 1 - (1 - x) * (1 - x) * (1 - x);
}

// Where the animation is: line (0..1 of its full width) and text (0..1 out of the line).
struct Pose {
  float line, text;
  int phase;  // 0 growing, 1 sliding out, 2 holding, 3 sliding in, 4 shrinking, 5 done
};

Pose At(float e, float total) {
  const float in_end = kLineTime + kSlideTime;
  // Backwards at the same speed as on the way in, ending with the countdown;
  // the hold fills the time in between (a very short countdown has none).
  const float out_len = std::max(0.0f, std::min(in_end, total - in_end));
  const float out_start = total - out_len;
  const float out_text = out_len * kSlideTime / in_end;
  if (e < 0) return {0, 0, 0};
  if (e < kLineTime) return {EaseOut(e / kLineTime), 0, 0};
  if (e < in_end) return {1, EaseOut((e - kLineTime) / kSlideTime), 1};
  if (e < out_start) return {1, 1, 2};
  if (e < out_start + out_text) return {1, EaseOut(1 - (e - out_start) / out_text), 3};
  if (e < total) return {EaseOut(1 - (e - out_start - out_text) / std::max(0.001f, out_len - out_text)), 0, 4};
  return {0, 0, 5};
}

HFONT TitleFont(LONG height) {
  static HFONT font = nullptr;
  static LONG font_h = 0;
  if (font && font_h == height) return font;
  // Fonts are kept (GPU text caches measurements per font), one per size seen.
  LOGFONTA lf{};
  lf.lfHeight = -height;
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
  lf.lfQuality = ANTIALIASED_QUALITY;
  const char* face = HudFontFace();
  std::snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "%s", face[0] ? face : "Arial");
  if (!face[0]) lf.lfWeight = FW_BOLD;
  font = CreateFontIndirectA(&lf);
  font_h = height;
  GpuTextKeepFont(font);
  Log("race title: font \"%s\" %ld px", lf.lfFaceName, height);
  return font;
}

void Text(HDC dc, const std::string& s, LONG x, LONG y, COLORREF c) {
  SetTextColor(dc, c);
  GdiCall call{};
  call.dc = dc;
  call.fn = GdiFn::kTextOut;
  call.s = s.c_str();
  call.len = static_cast<int>(s.size());
  call.x = x;
  call.y = y;
  GpuTextRun(call);
}

void Fill(HDC dc, const RECT& r, HBRUSH b) {
  if (r.right <= r.left || r.bottom <= r.top) return;
  GdiCall call{};
  call.dc = dc;
  call.fn = GdiFn::kFillRect;
  call.rect = &r;
  call.brush = b;
  GpuTextRun(call);
}

// One banner: the line, `name` above it and `sub` below it, as far open as `p` says.
void DrawBanner(Surface* rt, const std::string& name, const std::string& sub, const Pose& p, float line_pos,
                const char* tag, unsigned frames, float e, float total, float left, double& last_trace) {
  if (p.line <= 0.001f) return;
  const UINT W = rt->Width(), H = rt->Height();
  const Config& cfg = GetConfig();
  const double now = Now();

  HDC dc = rt->BeginGpuText();
  if (!dc) {
    if (++g.no_dc <= 5) Log("%s: no GPU text DC for the %ux%u render target", tag, W, H);
    return;
  }
  const float k = cfg.race_title_size;
  const LONG font_h = std::max<LONG>(12, std::lround(H * 0.062f * k));
  GpuTextSelectFont(dc, TitleFont(font_h));
  SetBkMode(dc, TRANSPARENT);
  SetTextCharacterExtra(dc, 0);
  TEXTMETRICA tm{};
  GpuTextMetrics(dc, &tm);
  const SIZE tw = GpuTextExtent(dc, name.c_str(), static_cast<int>(name.size()));
  const SIZE sw = GpuTextExtent(dc, sub.c_str(), static_cast<int>(sub.size()));
  const LONG cx = static_cast<LONG>(W / 2);
  const LONG line_y = std::lround(H * line_pos);
  const LONG thick = std::max<LONG>(2, std::lround(H * 0.0045f * k));
  const LONG shadow = std::max<LONG>(1, std::lround(H * 0.0028f * k));
  const LONG gap = std::lround(H * 0.016f * k);
  const LONG full = std::lround(std::max(tw.cx, sw.cx) * 1.3f);
  const LONG half = std::max<LONG>(1, std::lround(full * 0.5f * p.line));
  const LONG cap = tm.tmAscent - tm.tmInternalLeading;  // height of capitals above the baseline
  // Final baselines: name just above the line, subtitle's capitals just below.
  const LONG name_base = line_y - gap - tm.tmDescent / 2;
  const LONG sub_base = line_y + thick + gap + cap;
  // Out of the line: the name starts with its top under the line, the subtitle above it.
  const LONG sub_travel = sub_base - (line_y + thick) + tm.tmDescent;
  const LONG nb = name_base + std::lround((1 - p.text) * (line_y - name_base + tm.tmAscent));
  const LONG sb = sub_base - std::lround((1 - p.text) * sub_travel);

  static HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
  static HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
  const COLORREF shade = RGB(0, 0, 0), ink = RGB(255, 255, 255);
  SetTextAlign(dc, TA_CENTER | TA_BASELINE | TA_NOUPDATECP);
  if (p.text > 0.001f) {
    // Name: only above the line; subtitle: only below it.
    const RECT above{0, 0, static_cast<LONG>(W), line_y};
    const RECT below{0, line_y + thick, static_cast<LONG>(W), static_cast<LONG>(H)};
    GpuTextSetClip(dc, &above);
    if (!name.empty()) {
      Text(dc, name, cx + shadow, nb + shadow, shade);
      Text(dc, name, cx, nb, ink);
    }
    GpuTextSetClip(dc, &below);
    if (!sub.empty()) {
      Text(dc, sub, cx + shadow, sb + shadow, shade);
      Text(dc, sub, cx, sb, ink);
    }
  }
  GpuTextSetClip(dc, nullptr);
  Fill(dc, RECT{cx - half + shadow, line_y + shadow, cx + half + shadow, line_y + thick + shadow}, black);
  Fill(dc, RECT{cx - half, line_y, cx + half, line_y + thick}, white);
  GpuTextSelectFont(dc, GetStockObject(SYSTEM_FONT));
  GpuTextEndDC(dc);

  if (Logging() && (frames <= 3 || now - last_trace >= 0.25)) {
    last_trace = now;
    Log("%s: frame %u e=%.3f/%.2f left=%.3f phase %d line %.2f text %.2f; screen %ux%u font %ld (asc %ld "
        "desc %ld cap %ld) name %ld px sub %ld px, line y %ld x %ld..%ld thick %ld, name base %ld (final %ld), "
        "sub base %ld (final %ld)",
        tag, frames, e, total, left, p.phase, p.line, p.text, W, H, font_h, tm.tmAscent, tm.tmDescent, cap,
        tw.cx, sw.cx, line_y, cx - half, cx + half, thick, nb, name_base, sb, sub_base);
  }
}

}  // namespace

void InstallRaceTitleHooks() {
  const Config& c = GetConfig();
  if (!c.race_title && !c.announcer) {
    Log("race title: off (RaceTitle=0, Announcer=0), the game's corner text stays");
    return;
  }
  int ok = PatchCall(kNameAddCall, kQueueAdd, reinterpret_cast<const void*>(&NameAddHook)) ? 1 : 0;
  int calls = 0;
  for (uintptr_t site : kCountdownCalls)
    calls += PatchCall(site, kCountdownInit, reinterpret_cast<const void*>(&CountdownInitHook)) ? 1 : 0;
  int qlog = 0;
  if (c.race_title_log)
    for (uintptr_t site : kQueueAddCalls)
      qlog += PatchCall(site, kQueueAdd, reinterpret_cast<const void*>(&QueueAddLogHook)) ? 1 : 0;
  if (qlog) Log("race title: %d text queue adds logged", qlog);
  const int finish = c.race_title && PatchCall(kFinishAddCall, kQueueAdd, reinterpret_cast<const void*>(&FinishAddHook)) ? 1 : 0;
  Log("race title: podium finish text hook %s", finish ? "applied" : (c.race_title ? "MISSING" : "off (RaceTitle=0)"));
  int drops = 0;
  for (uintptr_t site : kDropStartCalls)
    drops += PatchCall(site, kDropStart, reinterpret_cast<const void*>(&DropStartHook)) ? 1 : 0;
  Log("race title: %s (size %.2f), announcer %s (volume %d%%); race name hook %s, %d of %d countdown calls hooked, %d of %d FINAL LAP starts "
      "hooked",
 c.race_title ? "on" : "off", c.race_title_size, c.announcer ? "on" : "off", c.announcer_volume,
      ok ? "applied" : "MISSING", calls,
      static_cast<int>(sizeof(kCountdownCalls) / sizeof(kCountdownCalls[0])), drops,
      static_cast<int>(sizeof(kDropStartCalls) / sizeof(kDropStartCalls[0])));
}

void RaceTitleNewFrame() { g.drawn_this_frame = false; }

namespace {

void CountdownBanner(Surface* rt) {
  if (!g.active || g.name.empty()) return;
  const Config& cfg = GetConfig();
  const UINT W = rt->Width(), H = rt->Height();
  const double now = Now();
  float e = 0;
  if (g.real_time) {
    e = static_cast<float>(now - g.start_clock);
  } else {
    // The race object stays alive through the countdown; stop if it looks
    // gone (vtable changed, countdown out of range, or frozen for a minute).
    const uintptr_t race = g.race;
    const uintptr_t vt = Rd<uintptr_t>(race);
    const float left = Rd<float>(race + kCountdownLeft);
    const bool frozen = g.last_left == left && now - g.last_change > 60.0;
    if (vt != g.vtable || !(left >= -1.0f && left <= g.total + 0.5f) || frozen) {
      ++g.lost;
      Log("race title: race %08x no longer counting down (vtable %08x was %08x, left %.3f of %.2f%s) after %u "
          "frames; stopped",
          static_cast<unsigned>(race), static_cast<unsigned>(vt), static_cast<unsigned>(g.vtable), left, g.total,
          frozen ? ", unchanged for 60 s" : "", g.frames);
      g.active = false;
      return;
    }
    if (left != g.last_left) {
      g.last_left = left;
      g.last_change = now;
    }
    // The game counts down once per simulation step: in between, move on by
    // the time since, at most one step (so a pause holds still).
    const float step = cfg.sim_rate > 0 ? 1.0f / cfg.sim_rate : 0.05f;
    const float est = left - std::min(step, static_cast<float>(now - g.last_change));
    e = g.total - est;
    if (left <= 0) e = g.total;
  }
  const Pose p = At(e, g.total);
  if (p.phase != g.phase) {
    if (Logging())
      Log("race title: phase %d -> %d at %.3f s of %.2f (frame %u, screen %ux%u)", g.phase, p.phase, e, g.total,
          g.frames, W, H);
    g.phase = p.phase;
  }
  if (p.phase == 5) {
    Log("race title: done after %u frames (%.2f s)", g.frames, e);
    g.active = false;
    return;
  }
  ++g.frames;
  DrawBanner(rt, g.name, g.sub, p, 0.352f, "race title", g.frames, e, g.total, g.last_left, g.last_trace);
}

// FINAL LAP: the same banner with only the game's text above the line, on
// the clock (open, hold kFinalLapHold, close).
void FinalLapBanner(Surface* rt) {
  if (!fl.active) return;
  const float total = 2 * (kLineTime + kSlideTime) + kFinalLapHold;
  const float e = static_cast<float>(Now() - fl.start);
  const Pose p = At(e, total);
  if (p.phase != fl.phase) {
    if (Logging())
      Log("final lap: phase %d -> %d at %.3f s of %.2f (frame %u, screen %ux%u)", fl.phase, p.phase, e, total,
          fl.frames, rt->Width(), rt->Height());
    fl.phase = p.phase;
  }
  if (p.phase == 5) {
    Log("final lap: done after %u frames (%.2f s)", fl.frames, e);
    fl.active = false;
    return;
  }
  ++fl.frames;
  DrawBanner(rt, fl.text, std::string(), p, 0.352f, "final lap", fl.frames, e, total, 0, fl.last_trace);
}

// Podium: wait for the cutscene after the finish text, then draw the banner
// near the top of the screen, open/hold/close over the cutscene's 7 s.
void PodiumBanner(Surface* rt) {
  if (!pd.pending && !pd.active) return;
  const uintptr_t game = Rd<uintptr_t>(kGame);
  const double now = Now();
  const int on = game ? Rd<int>(game + kPodium) : 0;
  if (pd.pending) {
    if (on == 1) {
      pd.pending = false;
      pd.active = true;
      pd.started = now;
      pd.last_e = -1;
      pd.last_change = now;
      pd.phase = -1;
      pd.frames = 0;
      Log("podium: cutscene started %.2f s after the finish text; banner \"%s\"", now - pd.captured,
          pd.text.c_str());
    } else if (now - pd.captured > kPodiumWait) {
      pd.pending = false;
      ++pd.given_back;
      // The event (and its HUD text queue) is deleted when the race ends
      // (0x45e170: delete [game+0x558], then 0). Without a podium the game
      // goes to the results screen ~0.2 s after the finish, so by now the
      // queue is usually freed memory: adding to it frees a stale node and
      // corrupts the heap (crash loading a later race). Only give the text
      // back while the game's own lookup (0x45e350: the first of the event
      // objects game+0x558..0x568, its +0x6c) still finds that same queue.
      const uintptr_t live = game ? reinterpret_cast<uintptr_t (*)()>(kCurrentQueue)() : 0;
      if (game && live && live == pd.queue) {
        Log("podium: no cutscene %.2f s after the finish text (flag %d); \"%s\" given back to text queue %08x",
            now - pd.captured, on, pd.text.c_str(), static_cast<unsigned>(pd.queue));
        reinterpret_cast<QueueAddFn>(kQueueAdd)(pd.queue, nullptr, pd.item);
      } else if (game) {
        Log("podium: no cutscene %.2f s after the finish text (flag %d); the race's text queue %08x is gone "
            "(current queue %08x): \"%s\" dropped, not given back",
            now - pd.captured, on, static_cast<unsigned>(pd.queue), static_cast<unsigned>(live), pd.text.c_str());
      } else {
        Log("podium: no cutscene and no game object; \"%s\" dropped", pd.text.c_str());
      }
      return;
    } else {
      return;
    }
  }
  if (on != 1) {
    Log("podium: cutscene ended (flag %d) after %u frames, %.2f s; banner off", on, pd.frames, now - pd.started);
    pd.active = false;
    return;
  }
  const float t = Rd<float>(game + kPodium4);
  if (!(t >= -0.5f && t <= kPodiumLength + 5.0f)) {
    Log("podium: cutscene timer +3434 = %g out of range; banner off", t);
    pd.active = false;
    return;
  }
  if (t != pd.last_e) {
    pd.last_e = t;
    pd.last_change = now;
  }
  // Game time steps once per simulation step: move on by at most one step.
  const Config& cfg = GetConfig();
  const float step = cfg.sim_rate > 0 ? 1.0f / cfg.sim_rate : 0.05f;
  const float e = std::min(kPodiumLength, t + std::min(step, static_cast<float>(now - pd.last_change)));
  const Pose p = At(e, kPodiumLength);
  if (p.phase != pd.phase) {
    if (Logging())
      Log("podium: phase %d -> %d at %.3f s of %.2f (frame %u, screen %ux%u)", pd.phase, p.phase, e, kPodiumLength,
          pd.frames, rt->Width(), rt->Height());
    pd.phase = p.phase;
  }
  if (p.phase == 5) return;  // closed; wait for the flag to clear
  ++pd.frames;
  DrawBanner(rt, pd.text, std::string(), p, kPodiumLineY, "podium", pd.frames, e, kPodiumLength, t, pd.last_trace);
}

// Announcer: podium.wav as the podium cutscene starts (game+0x3430 0 -> 1).
void PodiumSoundTick() {
  static int last = 0;
  const uintptr_t game = Rd<uintptr_t>(kGame);
  const int on = game ? Rd<int>(game + kPodium) : 0;
  if (on == last) return;
  last = on;
  if (on == 1 && GetConfig().announcer) OneShotPlay(kPodiumWav, "podium cutscene started");
}

// Once per frame: a voice as the countdown passes 5, 4, 3, 2 and 1 seconds
// left (only those within the countdown), the air horn when it runs out.
void AnnouncerTick() {
  OneShotTick();
  if (!g.snd_active) return;
  const uintptr_t race = g.race;
  const float left = Rd<float>(race + kCountdownLeft);
  if (Rd<uintptr_t>(race) != g.vtable || !(left >= -1.0f && left <= g.total + 0.5f)) {
    Log("announcer: race %08x no longer counting down (left %.3f); stopped", static_cast<unsigned>(race), left);
    g.snd_active = false;
    return;
  }
  const float prev = g.snd_prev;
  if (left >= prev) return;
  g.snd_prev = left;
  for (int n = 5; n >= 1; --n) {
    if (prev > n && left <= n) {
      char why[64];
      std::snprintf(why, sizeof(why), "countdown %.3f s left, was %.3f", left, prev);
      OneShotPlay(kCountWav[n - 1], why);
    }
  }
  if (left <= 0) {
    char why[64];
    std::snprintf(why, sizeof(why), "countdown over (%.3f, was %.3f)", left, prev);
    OneShotPlay(kGoWav, why);
    g.snd_active = false;
  }
}

}  // namespace

void RaceTitleDraw(Surface* rt) {
  if (!rt || g.drawn_this_frame) return;
  if (rt->Width() < 320 || rt->Height() < 200) return;  // not the screen
  g.drawn_this_frame = true;
  PodiumWatch();
  AnnouncerTick();
  PodiumSoundTick();
  if (!g.active && !fl.active && !pd.pending && !pd.active) return;
  const Config& cfg = GetConfig();
  if (!cfg.race_title || !GpuTextActive()) return;
  CountdownBanner(rt);
  FinalLapBanner(rt);
  PodiumBanner(rt);
}

}  // namespace dd
