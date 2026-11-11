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
// of it, they hold, and the same runs backwards so it is gone when the
// countdown ends.
//
// Countdown: BikeRace (= [[game+0x558]+0x34], 0x45e300) +0x15c is the time
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
constexpr uintptr_t kCountdownLeft = 0x15C;  // float seconds
constexpr uintptr_t kRaceStarted = 0x18A;    // byte
constexpr uintptr_t kMode = 0x2D70, kEvent = 0x2D74, kLaps = 0x2D90, kStuntEvent = 0x55C, kBaja = 0x560;
constexpr uintptr_t kTimeLimitMinutes = 0x2EB0;  // float (stunt panel 0x51f460: limit * 60 - elapsed)
constexpr uintptr_t kOnline = 0x3428;

// Timing (seconds).
constexpr float kLineTime = 0.30f;   // dot -> full line
constexpr float kSlideTime = 0.40f;  // text out of the line

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
} g;

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
  g.total = left;
  g.real_time = !(left > 0.5f && left < 60.0f);
  if (g.real_time) g.total = kLineTime + kSlideTime + GetConfig().race_title_hold + kLineTime + kSlideTime;
  g.start_clock = Now();
  g.last_left = -1;
  g.last_change = g.start_clock;
  g.phase = -1;
  g.frames = 0;
  g.active = true;  // the name may come later (the HUD can be built after the countdown is set)
  const uintptr_t game = Rd<uintptr_t>(kGame);
  char why[160] = {};
  g.sub = game ? Subtitle(game, why, sizeof(why)) : std::string();
  Log("race title: countdown set on race %08x (current %08x): %.2f s%s; title \"%s\", subtitle \"%s\" (%s)%s",
      static_cast<unsigned>(race), static_cast<unsigned>(CurrentRace()), left,
      g.real_time ? " - no countdown, playing on the clock" : "", g.name.c_str(), g.sub.c_str(), why,
      g.name.empty() ? " - no name captured yet" : "");
  LogGame("at countdown");
}

void __fastcall CountdownInitHook(uintptr_t race, void* edx) {
  reinterpret_cast<InitFn>(kCountdownInit)(race, edx);
  if (GetConfig().race_title) Start(race);
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
  const float hold = GetConfig().race_title_hold;
  const float in_end = kLineTime + kSlideTime;
  // Backwards: from the end of the hold to the end of the countdown, line
  // and text in the same proportion as on the way in. A short countdown
  // shortens the hold instead.
  float out_len = total - in_end - hold;
  if (out_len < in_end) out_len = std::max(0.0f, std::min(in_end, total - in_end));
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

}  // namespace

void InstallRaceTitleHooks() {
  const Config& c = GetConfig();
  if (!c.race_title) {
    Log("race title: off (RaceTitle=0), the game's corner text stays");
    return;
  }
  int ok = PatchCall(kNameAddCall, kQueueAdd, reinterpret_cast<const void*>(&NameAddHook)) ? 1 : 0;
  int calls = 0;
  for (uintptr_t site : kCountdownCalls)
    calls += PatchCall(site, kCountdownInit, reinterpret_cast<const void*>(&CountdownInitHook)) ? 1 : 0;
  Log("race title: on (hold %.2f s, size %.2f); race name hook %s, %d of %d countdown calls hooked",
      c.race_title_hold, c.race_title_size, ok ? "applied" : "MISSING", calls,
      static_cast<int>(sizeof(kCountdownCalls) / sizeof(kCountdownCalls[0])));
}

void RaceTitleNewFrame() { g.drawn_this_frame = false; }

void RaceTitleDraw(Surface* rt) {
  if (!g.active || !rt || g.drawn_this_frame || g.name.empty()) return;
  const Config& cfg = GetConfig();
  if (!cfg.race_title || !GpuTextActive()) return;
  const UINT W = rt->Width(), H = rt->Height();
  if (W < 320 || H < 200) return;  // not the screen
  g.drawn_this_frame = true;
  const double now = Now();
  float e = 0;
  if (g.real_time) {
    e = static_cast<float>(now - g.start_clock);
  } else {
    const uintptr_t race = CurrentRace();
    if (race != g.race) {
      ++g.lost;
      Log("race title: race %08x is no longer current (now %08x) after %u frames; stopped",
          static_cast<unsigned>(g.race), static_cast<unsigned>(race), g.frames);
      g.active = false;
      return;
    }
    const float left = Rd<float>(race + kCountdownLeft);
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
  if (p.line <= 0.001f) return;

  HDC dc = rt->BeginGpuText();
  if (!dc) {
    if (++g.no_dc <= 5) Log("race title: no GPU text DC for the %ux%u render target", W, H);
    return;
  }
  const float k = cfg.race_title_size;
  const LONG font_h = std::max<LONG>(12, std::lround(H * 0.062f * k));
  GpuTextSelectFont(dc, TitleFont(font_h));
  SetBkMode(dc, TRANSPARENT);
  SetTextCharacterExtra(dc, 0);
  TEXTMETRICA tm{};
  GpuTextMetrics(dc, &tm);
  const SIZE tw = GpuTextExtent(dc, g.name.c_str(), static_cast<int>(g.name.size()));
  const SIZE sw = GpuTextExtent(dc, g.sub.c_str(), static_cast<int>(g.sub.size()));
  const LONG cx = static_cast<LONG>(W / 2);
  const LONG line_y = std::lround(H * 0.352f);
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
    if (!g.name.empty()) {
      Text(dc, g.name, cx + shadow, nb + shadow, shade);
      Text(dc, g.name, cx, nb, ink);
    }
    GpuTextSetClip(dc, &below);
    if (!g.sub.empty()) {
      Text(dc, g.sub, cx + shadow, sb + shadow, shade);
      Text(dc, g.sub, cx, sb, ink);
    }
  }
  GpuTextSetClip(dc, nullptr);
  Fill(dc, RECT{cx - half + shadow, line_y + shadow, cx + half + shadow, line_y + thick + shadow}, black);
  Fill(dc, RECT{cx - half, line_y, cx + half, line_y + thick}, white);
  GpuTextSelectFont(dc, GetStockObject(SYSTEM_FONT));
  GpuTextEndDC(dc);

  if (Logging() && (g.frames <= 3 || now - g.last_trace >= 0.25)) {
    g.last_trace = now;
    Log("race title: frame %u e=%.3f/%.2f left=%.3f phase %d line %.2f text %.2f; screen %ux%u font %ld (asc %ld "
        "desc %ld cap %ld) name %ld px sub %ld px, line y %ld x %ld..%ld thick %ld, name base %ld (final %ld), "
        "sub base %ld (final %ld)",
        g.frames, e, g.total, g.last_left, p.phase, p.line, p.text, W, H, font_h, tm.tmAscent, tm.tmDescent, cap,
        tw.cx, sw.cx, line_y, cx - half, cx + half, thick, nb, name_base, sb, sub_base);
  }
}

}  // namespace dd
