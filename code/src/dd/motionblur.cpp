// MotionBlur=1: speed-based motion blur of the race scene.
//
// A zoom (radial) blur centred on the point the player's bike is heading
// for, whose length grows with the bike's speed between MotionBlurMinSpeed
// and MotionBlurMaxSpeed. Only the outer part of the picture streaks
// (outside MotionBlurCenter), so the rider and the track ahead stay clear
// while the edges stream past. The streak is evenly sampled (no noise) and
// trails off, so it reads as a line rather than a soft blur. It is a post-process of the finished 3D scene only:
//
//  - Where: every draw is noted (Device::Submit). The first screen-space draw
//    without a depth test after the frame's 3D scene (16 or more 3D draws
//    into the target that has the depth buffer) is the start of the HUD
//    (speedometer, panels, radar, name labels); the scene is blurred right
//    before it. GPU text, HUD text and the race title are drawn later still.
//  - Speed: the player's bike velocity (+0x64, feet per second, the vector
//    the speedometer's speed comes from), copied when the game simulates the
//    bike (the player's bike: call 0x497be8 into 0x40d820; computer riders:
//    0x497bd7 into 0x414000; see framerate.cpp). Read only: the game's
//    physics is not touched.
//  - Focus: that velocity's direction projected with the view and
//    projection the scene was drawn with (smoothed ones with SimSmooth), so
//    it follows the camera at any frame rate.
//  - Timing: speed and focus are filtered over real time (about 0.1 s), so
//    30 simulation steps a second don't show as steps, and the blur length
//    is a fraction of the screen, not a per-frame distance: it looks the same
//    at 60 or 240 Hz.
//
//  - The race direction arrow (VisualCue) is a 3D object in the scene, so it
//    would be blurred with it. Its spot in front of the camera comes from
//    fixes_arrow.cpp; 3D draws whose object sits there are kept back (their
//    vertices and the device state copied) and drawn right after the blur.
//
// Direct3D 11: one full-screen pass (shader below) from a copy of the scene
// (resolved from the MSAA buffer when antialiasing is on) back into it.
// Direct3D 9 (fixed function): the copy is drawn again 7 times, each zoomed a
// little further, averaged by blending (ZoomBlur9).
#include "motionblur.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../patch.h"
#include "backend9.h"
#include "device.h"
#include "msaa.h"
#include "surface.h"

namespace dd {
namespace {

constexpr float kFeetPerSecToMph = 0.68182f;
constexpr float kBaseLength = 0.08f;  // MotionBlurStrength=100 at full speed: 8% of the way to the focus
constexpr unsigned kMin3D = 16;       // 3D draws before a screen draw counts as the HUD
constexpr double kStaleSecs = 0.2;    // no bike step for this long (pause, replay, menus): no blur
constexpr int kLayers9 = 8;
constexpr double kArrowStaleSecs = 0.5;  // arrow spot older than this: no arrow in the race
constexpr float kArrowMatch = 0.3f;      // how near the spot (fraction of its distance) a draw must be
constexpr DWORD kArrowMaxVerts = 2000;   // bigger draws are not the arrow

LONGLONG Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double QpcFreq() {
  static double f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return static_cast<double>(q.QuadPart);
  }();
  return f;
}

// The player's bike, as last simulated.
struct BikeSample {
  const void* bike = nullptr;
  float vel[3] = {};
  float pos[3] = {};
  bool crashed = false;
  LONGLONG at = 0;
  unsigned steps = 0;
} g_bike;
const void* g_race_seen = nullptr;  // BikeRace pointer last checked readable

// Per-frame state (one Device draws the races).
struct FrameState {
  unsigned flips = ~0u;
  unsigned n3d = 0;          // 3D draws into the scene target this frame
  unsigned tlz = 0;          // screen-space draws with depth test (particles?)
  bool done = false;         // blur placed this frame
  bool blurred = false;      // ... and drawn
  unsigned after3d = 0;      // 3D draws after the blur (the HUD start was misjudged)
  d3d7::D3DMATRIX view{}, proj{};
  d3d7::D3DVIEWPORT7 vp{};
  bool have_mats = false;
  UINT rt_w = 0, rt_h = 0;
  bool arrow_spot = false;   // the arrow's spot was known this frame
  unsigned arrow_draws = 0;  // draws kept back as the arrow
  float arrow_best = 1e9f;   // nearest a 3D draw came to the spot (MotionBlurLog)
} g_frame;

// The arrow's spot in front of the camera (fixes_arrow.cpp).
struct ArrowSpot {
  float h = 0, v = 0, dist = 0, size = 0;
  LONGLONG at = 0;
} g_arrow;

// Draws kept back until after the blur, with the device state they had.
struct Deferred {
  DWORD prim = 0;
  FvfLayout layout{};
  DWORD count = 0;
  std::vector<uint8_t> verts;
  std::vector<WORD> idx;
  bool indexed = false;
  const Surface* rt = nullptr;
  DWORD rs[256];
  DWORD tss[8][32];
  Surface* tex[8];
  d3d7::D3DMATRIX world, view, proj;
  d3d7::D3DVIEWPORT7 vp;
  d3d7::D3DMATERIAL7 mat;
  std::map<DWORD, d3d7::D3DLIGHT7> lights;
  std::map<DWORD, BOOL> light_on;
};
std::vector<Deferred> g_deferred;  // reused; the first g_ndeferred are pending
size_t g_ndeferred = 0;
unsigned g_arrow_logged = 0;

// Filtered values.
struct Filter {
  float amount = 0.0f;  // 0..1 from speed
  float fu = 0.5f, fv = 0.5f;
  LONGLONG last = 0;
  bool have_focus = false;
} g_filt;

// GPU objects.
struct Gpu {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* cb = nullptr;
  bool failed = false;
  GpuTexture scratch;  // copy of the scene
  unsigned resets9 = ~0u;
} g_gpu;

// MotionBlurLog: 5 s windows.
struct Window {
  LONGLONG start = 0;
  unsigned frames3d = 0, blurred = 0, slow = 0, stale = 0, no_trigger = 0, no_focus = 0;
  unsigned after3d_frames = 0, after3d_draws = 0, tlz = 0;
  double mph_sum = 0, amount_sum = 0, len_sum = 0, fu_sum = 0, fv_sum = 0, fwd_sum = 0;
  float mph_max = 0, len_max = 0;
  unsigned samples = 0;
  double pass_us = 0;
  std::string trigger;  // texture of the HUD draw the blur went before
  unsigned spot_frames = 0, arrow_frames = 0, arrow_draws = 0, arrow_dropped = 0, arrow_late = 0;
  // The nearest miss in frames where the spot was known but no draw matched.
  float miss = 1e9f, miss_x = 0, miss_y = 0, miss_z = 0;
  DWORD miss_count = 0;
  std::string miss_tex;
} g_win;

// MotionBlurLog: whole-frame draw sequences (a few per run).
struct DumpEvent {
  int kind;  // 0 3D, 1 processed (3D), 2 screen + depth test, 3 screen, 4 BLUR, 5 arrow kept back, 6 arrow drawn
  std::string tex;
  unsigned n;
};
std::vector<DumpEvent> g_dump;
bool g_dumping = false;
int g_dumps = 0;
LONGLONG g_race_start = 0, g_last_dump = 0;
bool g_first_logged = false;
unsigned g_frames_without = 0;  // scene frames in a row with no HUD draw found

const char kShader[] = R"(
cbuffer P : register(b0) {
  float4 focus;  // xy = focus (0..1), z = blur length (fraction of the way to the focus), w = taps
  float4 shape;  // x = sharp radius, y = full radius (1 = middle of a screen edge)
  float4 px;     // xy = 1 / size, z = most blur in pixels
};
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  float4 here = src.SampleLevel(smp, i.uv, 0);
  // Distance from the middle with the screen as a circle (1 = edge middles):
  // only the outer part of the picture streaks.
  float r = length((i.uv - 0.5) * 2.0);
  float w = smoothstep(shape.x, shape.y, r);
  float2 v = (i.uv - focus.xy) * focus.z * w;  // towards the focus
  float lp = length(v / px.xy);
  if (lp < 0.75) return here;
  if (lp > px.z) v *= px.z / lp;
  // Evenly spaced taps along the streak (no noise, so it reads as a line),
  // weighted towards this pixel so the streak trails off.
  int n = (int)focus.w;
  float3 sum = 0;
  float wsum = 0;
  [loop] for (int k = 0; k < n; ++k) {
    float t = (k + 0.5) / n;
    float kw = 1.0 - 0.6 * t;
    sum += src.SampleLevel(smp, i.uv - v * t, 0).rgb * kw;
    wsum += kw;
  }
  return float4(sum / wsum, here.a);
}
)";

bool InitShaders() {
  if (g_gpu.ps) return true;
  if (g_gpu.failed) return false;
  Backend& b = B();
  ID3DBlob* vs = Compile(kShader, "vs_main", "vs_4_0");
  ID3DBlob* ps = Compile(kShader, "ps_main", "ps_4_0");
  bool ok = vs && ps &&
            SUCCEEDED(b.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_gpu.vs)) &&
            SUCCEEDED(b.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_gpu.ps));
  SafeRelease(vs);
  SafeRelease(ps);
  if (ok) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = SUCCEEDED(b.device->CreateBuffer(&bd, nullptr, &g_gpu.cb));
  }
  if (!ok) {
    g_gpu.failed = true;
    SafeRelease(g_gpu.vs);
    SafeRelease(g_gpu.ps);
    Log("motion blur: shader setup failed; motion blur off");
  }
  return ok;
}

bool EnsureScratch(UINT w, UINT h) {
  if (IsD3D9() && g_gpu.resets9 != ResetCount9()) {
    g_gpu.scratch.Reset();
    g_gpu.resets9 = ResetCount9();
  }
  GpuTexture& t = g_gpu.scratch;
  if (t.Valid() && t.width == w && t.height == h) return true;
  // D3D11: a plain texture to sample; D3D9: a render target so StretchRect
  // can copy into it.
  if (!CreateGpuTexture(t, w, h, 1, IsD3D9(), false, false)) {
    LogOnce("motion blur: scene copy %ux%u could not be made", w, h);
    return false;
  }
  Log("motion blur: scene copy %ux%u", w, h);
  return true;
}

// Rows times columns (row vectors: v' = v * M).
void Mul3(const float d[3], const d3d7::D3DMATRIX& m, float w, float out[4]) {
  for (int j = 0; j < 4; ++j) out[j] = d[0] * m.m[0][j] + d[1] * m.m[1][j] + d[2] * m.m[2][j] + w * m.m[3][j];
}

// A world point or direction (w = 0) to render-target 0..1 coordinates.
// Returns the clip w (<= 0: behind the camera).
float Project(const float p[3], float w, float* u, float* v) {
  float a[4], c[4];
  Mul3(p, g_frame.view, w, a);
  const float av[3] = {a[0], a[1], a[2]};
  Mul3(av, g_frame.proj, a[3], c);
  if (std::fabs(c[3]) < 1e-6f) return 0.0f;
  const float nx = c[0] / c[3], ny = c[1] / c[3];
  const d3d7::D3DVIEWPORT7& vp = g_frame.vp;
  const float x = vp.dwX + (nx + 1.0f) * 0.5f * vp.dwWidth;
  const float y = vp.dwY + (1.0f - ny) * 0.5f * vp.dwHeight;
  *u = g_frame.rt_w ? x / g_frame.rt_w : 0.5f;
  *v = g_frame.rt_h ? y / g_frame.rt_h : 0.5f;
  return c[3];
}

void FlushWindow(LONGLONG now) {
  Window& w = g_win;
  const double secs = (now - w.start) / QpcFreq();
  if (w.frames3d || w.stale) {
    const double n = w.samples ? w.samples : 1;
    patch::Log("motionblur: %.1fs: %u scene frames, %u blurred, %u below MinSpeed, %u bike not simulated (pause/replay), "
               "%u no HUD draw found, %u no focus; speed avg %.0f max %.0f mph; amount avg %.2f; length avg %.3f max %.3f; "
               "focus avg %.2f,%.2f forward %.2f; 3D draws after the blur: %u in %u frames; screen draws with depth test %u; "
               "pass %.0f us/frame; HUD starts with '%s'",
               secs, w.frames3d, w.blurred, w.slow, w.stale, w.no_trigger, w.no_focus, w.mph_sum / n, w.mph_max,
               w.amount_sum / n, w.blurred ? w.len_sum / w.blurred : 0.0, w.len_max, w.fu_sum / n, w.fv_sum / n,
               w.fwd_sum / n, w.after3d_draws, w.after3d_frames, w.tlz, w.blurred ? w.pass_us / w.blurred : 0.0,
               w.trigger.c_str());
    if (w.spot_frames || w.arrow_draws || w.arrow_dropped) {
      patch::Log("motionblur: arrow: spot known in %u frames, kept back and drawn after the blur in %u frames (%u draws), "
                 "%u dropped (frame ended without a draw point), %u frames with the spot but nothing matched%s",
                 w.spot_frames, w.arrow_frames, w.arrow_draws, w.arrow_dropped, w.spot_frames - w.arrow_frames,
                 w.arrow_late ? " (some after the blur: drawn sharp anyway)" : "");
      if (w.miss < 1e8f && w.spot_frames > w.arrow_frames)
        patch::Log("motionblur: arrow: nearest unmatched 3D draw %.2f (match < %.2f): %lu verts '%s' at view %.2f %.2f %.2f",
                   w.miss, kArrowMatch, w.miss_count, w.miss_tex.c_str(), w.miss_x, w.miss_y, w.miss_z);
    }
  }
  w = Window();
  w.start = now;
}

const char* KindName(int k) {
  switch (k) {
    case 0: return "3D";
    case 1: return "3D (processed)";
    case 2: return "screen+depthtest";
    case 3: return "screen";
    case 5: return "arrow (kept back)";
    case 6: return "arrow (drawn)";
    default: return "== MOTION BLUR ==";
  }
}

void DumpEventAdd(int kind, const Surface* tex) {
  const std::string name = tex ? tex->TextureName() : std::string();
  if (!g_dump.empty() && g_dump.back().kind == kind && (kind < 2 || g_dump.back().tex == name)) {
    ++g_dump.back().n;
    return;
  }
  if (g_dump.size() < 400) g_dump.push_back({kind, kind < 2 ? std::string() : name, 1});
}

// End of a frame: the counts that need the whole frame.
void EndFrame() {
  FrameState& f = g_frame;
  if (f.n3d >= kMin3D) {
    ++g_win.frames3d;
    if (!f.done) {
      ++g_win.no_trigger;
      if (++g_frames_without == 600)
        patch::Log("motionblur: 600 scene frames in a row with no HUD draw after the 3D scene; nothing blurred "
                   "(MotionBlurLog=1 lists the draws)");
    } else {
      g_frames_without = 0;
    }
    if (f.after3d) {
      ++g_win.after3d_frames;
      g_win.after3d_draws += f.after3d;
    }
    g_win.tlz += f.tlz;
    if (f.arrow_spot) ++g_win.spot_frames;
    if (f.arrow_draws) {
      ++g_win.arrow_frames;
      g_win.arrow_draws += f.arrow_draws;
    }
  }
  if (g_dumping) {
    g_dumping = false;
    patch::Log("motionblur: frame draw sequence (%u 3D draws, blur %s, %u 3D draws after it):", f.n3d,
               f.blurred ? "drawn" : f.done ? "placed, not drawn" : "not placed", f.after3d);
    for (const DumpEvent& e : g_dump)
      patch::Log("motionblur:   %-18s x%-4u %s", KindName(e.kind), e.n, e.tex.c_str());
    g_dump.clear();
  }
}

void NewFrameIfNeeded(UINT w, UINT h) {
  const unsigned flips = Counters().flips;
  if (flips == g_frame.flips) return;
  if (g_frame.flips != ~0u) EndFrame();
  if (g_ndeferred) {  // kept back and never drawn (no end of scene seen): gone
    g_win.arrow_dropped += static_cast<unsigned>(g_ndeferred);
    g_ndeferred = 0;
  }
  const LONGLONG now = Qpc();
  g_frame = FrameState();
  g_frame.flips = flips;
  g_frame.rt_w = w;
  g_frame.rt_h = h;
  if (GetConfig().motion_blur_log) {
    if (!g_win.start) g_win.start = now;
    if (now - g_win.start >= static_cast<LONGLONG>(5 * QpcFreq())) FlushWindow(now);
    // Dump a frame 10 s into blurring and then every 30 s, three at most.
    const bool racing = g_bike.at && (now - g_bike.at) < static_cast<LONGLONG>(kStaleSecs * QpcFreq());
    if (!racing) g_race_start = 0;
    else if (!g_race_start) g_race_start = now;
    if (racing && g_dumps < 3 && now - g_race_start > static_cast<LONGLONG>(10 * QpcFreq()) &&
        (!g_last_dump || now - g_last_dump > static_cast<LONGLONG>(30 * QpcFreq()))) {
      g_dumping = true;
      g_last_dump = now;
      ++g_dumps;
      g_dump.clear();
    }
  }
}

}  // namespace

void MotionBlurBikeStep(const void* bike) {
  if (!bike) return;
  const uint8_t* b = static_cast<const uint8_t*>(bike);
  // The player's bike: BikeRace (bike+0x740) +0x38, as in fixes_ai.cpp.
  const void* race = *reinterpret_cast<const void* const*>(b + 0x740);
  if (!race) return;
  if (race != g_race_seen) {
    if (IsBadReadPtr(race, 0x40)) return;
    g_race_seen = race;
  }
  const void* player = *reinterpret_cast<const void* const*>(static_cast<const uint8_t*>(race) + 0x38);
  const bool ai = b[0x734] != 0;  // computer rider flag
  static unsigned s_seen = 0;
  if (s_seen < 12) {
    ++s_seen;
    patch::Log("motionblur: bike step %p race %p race+0x38 %p ai %d crashed %d", bike, race, player, ai ? 1 : 0,
               *reinterpret_cast<const int*>(b + 0x444) != 0 ? 1 : 0);
  }
  // The player: BikeRace+0x38, or else the one bike that isn't a computer rider.
  if (player != bike && ai) return;
  if (g_bike.bike != bike) {
    patch::Log("motionblur: player's bike %p (race %p, race+0x38 %s)", bike, race,
               player == bike ? "matches" : "differs");
  }
  g_bike.bike = bike;
  std::memcpy(g_bike.vel, b + 0x64, sizeof(g_bike.vel));
  std::memcpy(g_bike.pos, b + 0xc, sizeof(g_bike.pos));
  g_bike.crashed = *reinterpret_cast<const int*>(b + 0x444) != 0;
  g_bike.at = Qpc();
  ++g_bike.steps;
}

void MotionBlurDeviceLost() {
  g_gpu.scratch.Reset();
}

void MotionBlurArrowSpot(float h, float v, float dist, float size) {
  if (!MotionBlurOn()) return;
  if (!std::isfinite(h) || !std::isfinite(v) || !std::isfinite(dist) || !(std::fabs(dist) > 1e-3f)) return;
  if (!g_arrow.at && GetConfig().motion_blur_log)
    patch::Log("motionblur: arrow spot h %.3f v %.3f dist %.3f size %.3f", h, v, dist, size);
  g_arrow.h = h;
  g_arrow.v = v;
  g_arrow.dist = dist;
  g_arrow.size = size;
  g_arrow.at = Qpc();
}

bool Device::MotionBlurNote(const Draw& d, bool tl) {
  if (!rt_ || !rt_->ZBuffer()) return false;  // menus, offscreen pictures: not the race scene
  NewFrameIfNeeded(rt_->Width(), rt_->Height());
  FrameState& f = g_frame;
  const bool processed = tl && d.vb && d.vb->HasClipData(d.first, d.count);
  if (!tl || processed) {
    ++f.n3d;
    if (f.done) ++f.after3d;
    if (!tl) {
      f.view = view_;
      f.proj = proj_;
      f.vp = viewport_;
      f.have_mats = true;
      if (ArrowKeepBack(d)) return true;
    }
    if (g_dumping) DumpEventAdd(processed ? 1 : 0, nullptr);
    return false;
  }
  const bool depth_test = rs_[d3d7::RS_ZENABLE] && rs_[d3d7::RS_ZFUNC] != 8;  // 8 = D3DCMP_ALWAYS
  if (depth_test) {
    ++f.tlz;
    if (g_dumping) DumpEventAdd(2, tex_[0]);
    return false;
  }
  if (f.done || f.n3d < kMin3D) {
    if (g_dumping) DumpEventAdd(3, tex_[0]);
    return false;
  }
  f.done = true;
  // Whatever happens below (blurred or not), the arrow is drawn next.
  struct ReplayAfter {
    Device* dev;
    ~ReplayAfter() { dev->MotionBlurReplay(); }
  } replay_after{this};
  const Config& c = GetConfig();
  const LONGLONG now = Qpc();
  const double freq = QpcFreq();
  if (c.motion_blur_log && g_win.trigger.empty()) g_win.trigger = tex_[0] ? tex_[0]->TextureName() : "(no texture)";

  // Speed -> amount, filtered over real time.
  const bool stale = !g_bike.at || now - g_bike.at > static_cast<LONGLONG>(kStaleSecs * freq);
  const float dt = g_filt.last ? static_cast<float>((now - g_filt.last) / freq) : 0.0f;
  g_filt.last = now;
  const float k = dt > 0.25f || dt <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / 0.1f);
  float mph = 0.0f, fwd = 0.0f, fu = g_filt.fu, fv = g_filt.fv;
  bool focus_ok = false;
  if (stale) {
    ++g_win.stale;
    g_filt.amount = 0.0f;
    g_filt.have_focus = false;
  } else {
    const float* vel = g_bike.vel;
    const float fps = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    mph = fps * kFeetPerSecToMph;
    float t = (mph - c.motion_blur_min_speed) / static_cast<float>(c.motion_blur_max_speed - c.motion_blur_min_speed);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float target = t * t * (3.0f - 2.0f * t);
    // Focus: where the bike is heading, on screen.
    if (f.have_mats && fps > 1.0f) {
      const float dir[3] = {vel[0] / fps, vel[1] / fps, vel[2] / fps};
      float u = 0.5f, v = 0.5f;
      const float cw = Project(dir, 0.0f, &u, &v);
      const float m23 = std::fabs(f.proj.m[2][3]) > 1e-6f ? std::fabs(f.proj.m[2][3]) : 1.0f;
      fwd = cw / m23;  // cosine between the heading and the view direction
      if (fwd > 0.2f) {
        focus_ok = true;
        u = u < -0.5f ? -0.5f : u > 1.5f ? 1.5f : u;
        v = v < -0.5f ? -0.5f : v > 1.5f ? 1.5f : v;
        if (!g_filt.have_focus) {
          g_filt.fu = u;
          g_filt.fv = v;
          g_filt.have_focus = true;
        } else {
          g_filt.fu += (u - g_filt.fu) * k;
          g_filt.fv += (v - g_filt.fv) * k;
        }
      }
    }
    if (!focus_ok) {
      ++g_win.no_focus;
      target = 0.0f;  // heading sideways or at the camera: a zoom blur would look wrong
    } else {
      // Fade in as the heading turns towards the view.
      const float s = (fwd - 0.2f) / 0.4f;
      target *= s >= 1 ? 1 : s * s * (3.0f - 2.0f * s);
    }
    g_filt.amount += (target - g_filt.amount) * k;
    fu = g_filt.fu;
    fv = g_filt.fv;
  }
  const float len = g_filt.amount * kBaseLength * c.motion_blur_strength / 100.0f;
  if (c.motion_blur_log) {
    ++g_win.samples;
    g_win.mph_sum += mph;
    g_win.mph_max = std::max(g_win.mph_max, mph);
    g_win.amount_sum += g_filt.amount;
    g_win.fu_sum += fu;
    g_win.fv_sum += fv;
    g_win.fwd_sum += fwd;
  }
  if (len < 0.002f) {
    if (!stale) ++g_win.slow;
    if (g_dumping) DumpEventAdd(3, tex_[0]);
    return false;
  }
  // Sharp inside MotionBlurCenter (percent of the way to the screen edge),
  // full strength from a little past it to the edges and corners.
  const float inner = c.motion_blur_center / 100.0f;
  const float outer = inner + std::max(0.25f, (1.1f - inner) * 0.8f);
  const LONGLONG t0 = Qpc();
  const bool ok = MotionBlurPass(fu, fv, len, inner, outer);
  const LONGLONG t1 = Qpc();
  if (ok) {
    f.blurred = true;
    ++g_win.blurred;
    g_win.len_sum += len;
    g_win.len_max = std::max(g_win.len_max, len);
    g_win.pass_us += (t1 - t0) * 1e6 / freq;
  }
  if (g_dumping) {
    DumpEventAdd(4, nullptr);
    DumpEventAdd(3, tex_[0]);
  }
  if (!g_first_logged && ok) {
    g_first_logged = true;
    float bu = 0, bv = 0;
    const float bw = Project(g_bike.pos, 1.0f, &bu, &bv);
    patch::Log("motionblur: first blurred frame: %.0f mph, length %.3f, focus %.2f,%.2f (forward %.2f), "
               "bike on screen at %.2f,%.2f (w %.1f), after %u 3D draws, before '%s', target %ux%u, viewport %lu,%lu %lux%lu",
               mph, len, fu, fv, fwd, bu, bv, bw, f.n3d, tex_[0] ? tex_[0]->TextureName().c_str() : "(no texture)",
               f.rt_w, f.rt_h, f.vp.dwX, f.vp.dwY, f.vp.dwWidth, f.vp.dwHeight);
  }
  return false;
}

bool Device::ArrowKeepBack(const Draw& d) {
  FrameState& f = g_frame;
  if (!g_arrow.at || !d.verts || !d.count || d.count > kArrowMaxVerts || !d.layout.stride) return false;
  const LONGLONG now = Qpc();
  if (now - g_arrow.at > static_cast<LONGLONG>(kArrowStaleSecs * QpcFreq())) return false;
  f.arrow_spot = true;
  // Where the object is in front of the camera: its origin (the arrow's
  // position is set to the spot) and its first vertex, in view space.
  // Compared without signs, as the view's axes may point either way.
  const float dist = std::fabs(g_arrow.dist), ah = std::fabs(g_arrow.h), av = std::fabs(g_arrow.v);
  auto off = [&](const float p[4]) {
    const float dx = std::fabs(std::fabs(p[0]) - ah), dy = std::fabs(std::fabs(p[1]) - av),
                dz = std::fabs(std::fabs(p[2]) - dist);
    return std::max(dx, std::max(dy, dz)) / dist;
  };
  const float origin[3] = {world_.m[3][0], world_.m[3][1], world_.m[3][2]};
  float a[4], w[4], b[4];
  Mul3(origin, view_, 1.0f, a);
  const float* p0 = reinterpret_cast<const float*>(d.verts + static_cast<size_t>(d.first) * d.layout.stride);
  Mul3(p0, world_, 1.0f, w);
  Mul3(w, view_, 1.0f, b);
  const float oa = off(a), ob = off(b);
  const float score = std::min(oa, ob);
  const float* at = oa <= ob ? a : b;
  const Config& c = GetConfig();
  if (score >= kArrowMatch) {
    if (c.motion_blur_log && score < g_win.miss && !f.arrow_draws) {
      g_win.miss = score;
      g_win.miss_x = at[0], g_win.miss_y = at[1], g_win.miss_z = at[2];
      g_win.miss_count = d.count;
      g_win.miss_tex = tex_[0] ? tex_[0]->TextureName() : "(no texture)";
    }
    return false;
  }
  if (c.motion_blur_log && g_arrow_logged < 12) {
    ++g_arrow_logged;
    patch::Log("motionblur: arrow draw: %lu verts %lu indices prim %lu '%s' z %lu/%lu blend %lu, view %.2f %.2f %.2f "
               "(%s), spot %.2f %.2f %.2f, off %.3f%s",
               d.count, d.idx ? d.nidx : 0, d.prim, tex_[0] ? tex_[0]->TextureName().c_str() : "(no texture)",
               rs_[d3d7::RS_ZENABLE], rs_[d3d7::RS_ZWRITEENABLE], rs_[d3d7::RS_ALPHABLENDENABLE], at[0], at[1],
               at[2], oa <= ob ? "origin" : "vertex", g_arrow.h, g_arrow.v, g_arrow.dist, score,
               f.done ? ", after the blur already" : "");
  }
  if (f.done) {  // the blur is already drawn: nothing to do
    ++g_win.arrow_late;
    return false;
  }
  ++f.arrow_draws;
  if (g_ndeferred == g_deferred.size()) g_deferred.emplace_back();
  Deferred& k = g_deferred[g_ndeferred++];
  const size_t stride = d.layout.stride;
  const uint8_t* src = d.verts + static_cast<size_t>(d.first) * stride;
  k.verts.assign(src, src + static_cast<size_t>(d.count) * stride);
  k.indexed = d.idx != nullptr;
  if (d.idx) k.idx.assign(d.idx, d.idx + d.nidx);
  else k.idx.clear();
  k.prim = d.prim;
  k.layout = d.layout;
  k.count = d.count;
  k.rt = rt_;
  std::memcpy(k.rs, rs_, sizeof(k.rs));
  std::memcpy(k.tss, tss_, sizeof(k.tss));
  std::memcpy(k.tex, tex_, sizeof(k.tex));
  k.world = world_;
  k.view = view_;
  k.proj = proj_;
  k.vp = viewport_;
  k.mat = material_;
  k.lights = lights_;
  k.light_on = light_on_;
  if (g_dumping) DumpEventAdd(5, tex_[0]);
  return true;
}

void Device::MotionBlurReplay() {
  if (!g_ndeferred) return;
  const size_t n = g_ndeferred;
  g_ndeferred = 0;
  // The device's own state, put back afterwards.
  static Deferred s;
  std::memcpy(s.rs, rs_, sizeof(s.rs));
  std::memcpy(s.tss, tss_, sizeof(s.tss));
  std::memcpy(s.tex, tex_, sizeof(s.tex));
  s.world = world_;
  s.view = view_;
  s.proj = proj_;
  s.vp = viewport_;
  s.mat = material_;
  s.lights.swap(lights_);
  s.light_on.swap(light_on_);
  for (size_t i = 0; i < n; ++i) {
    Deferred& k = g_deferred[i];
    if (k.rt != rt_) {
      ++g_win.arrow_dropped;
      continue;
    }
    std::memcpy(rs_, k.rs, sizeof(rs_));
    std::memcpy(tss_, k.tss, sizeof(tss_));
    std::memcpy(tex_, k.tex, sizeof(tex_));
    world_ = k.world;
    view_ = k.view;
    proj_ = k.proj;
    viewport_ = k.vp;
    material_ = k.mat;
    lights_ = k.lights;
    light_on_ = k.light_on;
    wvp_dirty_ = true;
    Draw d;
    d.prim = k.prim;
    d.verts = k.verts.data();
    d.layout = k.layout;
    d.first = 0;
    d.count = k.count;
    d.idx = k.indexed ? k.idx.data() : nullptr;
    d.nidx = static_cast<DWORD>(k.idx.size());
    SubmitDraw(d);
    if (g_dumping) DumpEventAdd(6, tex_[0]);
  }
  std::memcpy(rs_, s.rs, sizeof(rs_));
  std::memcpy(tss_, s.tss, sizeof(tss_));
  std::memcpy(tex_, s.tex, sizeof(tex_));
  world_ = s.world;
  view_ = s.view;
  proj_ = s.proj;
  viewport_ = s.vp;
  material_ = s.mat;
  lights_.swap(s.lights);
  light_on_.swap(s.light_on);
  wvp_dirty_ = true;
}

bool Device::MotionBlurPass(float fu, float fv, float len, float inner, float outer) {
  if (IsD3D9()) {
    bool has_depth = false;
    if (!BindTarget9(has_depth, false) || !cur_rt9_) return false;
    GpuTexture* rt = rt_->GpuForDraw();
    if (!EnsureScratch(cur_rt9_w_, cur_rt9_h_)) return false;
    bool copied = cur_msaa_ && MsaaResolveCopy(rt->Native(), g_gpu.scratch);
    if (!copied) {
      EndScene9();
      copied = SUCCEEDED(B().dev9->StretchRect(cur_rt9_, nullptr, g_gpu.scratch.surf9, nullptr, D3DTEXF_NONE));
    }
    if (!copied) {
      LogOnce("motion blur: scene copy failed (Direct3D 9)");
      return false;
    }
    BlitParams p;
    p.dst9 = cur_rt9_;
    p.dst_w = cur_rt9_w_;
    p.dst_h = cur_rt9_h_;
    p.src9 = g_gpu.scratch.tex9;
    p.src_w = cur_rt9_w_;
    p.src_h = cur_rt9_h_;
    ZoomBlur9(p, fu, fv, len, inner, outer, kLayers9);
    if (cur_msaa_) MsaaMarkDrawn();
    rt_->MarkGpuWritten();
    return true;
  }
  if (!InitShaders()) return false;
  bool has_depth = false;
  if (!BindTarget(has_depth)) return false;
  GpuTexture* rt = rt_->GpuForDraw();
  const UINT w = rt->width, h = rt->height;
  if (!w || !h || !EnsureScratch(w, h)) return false;
  Backend& b = B();
  bool copied = cur_msaa_ && MsaaResolveCopy(rt->Native(), g_gpu.scratch);
  if (!copied && !cur_msaa_ && rt->tex) {
    b.ctx->CopySubresourceRegion(g_gpu.scratch.tex, 0, 0, 0, 0, rt->tex, 0, nullptr);
    copied = true;
  }
  if (!copied) {
    LogOnce("motion blur: scene copy failed");
    return false;
  }
  const float max_px = std::max(w, h) * 0.12f;
  // About one tap per 2 pixels of the longest streak, 8 to 32.
  float taps = std::ceil(std::min(max_px, len * 1.5f * std::max(w, h)) / 2.0f);
  taps = taps < 8 ? 8 : taps > 32 ? 32 : taps;
  const float cb[12] = {fu, fv, len, taps, inner, outer, static_cast<float>(w) / h, 0,
                        1.0f / w, 1.0f / h, max_px, 0};
  b.ctx->UpdateSubresource(g_gpu.cb, 0, nullptr, cb, 0, 0);
  ID3D11ShaderResourceView* none[2] = {};
  b.ctx->PSSetShaderResources(0, 2, none);
  b.ctx->OMSetRenderTargets(1, &cur_rtv_, nullptr);
  D3D11_VIEWPORT v{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
  b.ctx->RSSetViewports(1, &v);
  b.ctx->RSSetState(b.blit_raster);
  b.ctx->OMSetDepthStencilState(b.no_depth, 0);
  b.ctx->OMSetBlendState(b.no_blend, nullptr, 0xffffffff);
  b.ctx->IASetInputLayout(nullptr);
  b.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  b.ctx->VSSetShader(g_gpu.vs, nullptr, 0);
  b.ctx->PSSetShader(g_gpu.ps, nullptr, 0);
  b.ctx->PSSetConstantBuffers(0, 1, &g_gpu.cb);
  b.ctx->PSSetShaderResources(0, 1, &g_gpu.scratch.srv);
  b.ctx->PSSetSamplers(0, 1, &b.linear_sampler);
  b.ctx->Draw(3, 0);
  b.ctx->PSSetShaderResources(0, 1, none);
  ContextTouched();  // the device binds its own state again for the next draw
  if (cur_msaa_) MsaaMarkDrawn();
  rt_->MarkGpuWritten();
  return true;
}

}  // namespace dd
