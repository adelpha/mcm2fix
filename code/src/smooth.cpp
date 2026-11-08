// Smooth drawing between fixed-rate simulation steps.
//
// With SimRate the race only moves SimRate times a second, while frames are
// drawn at the monitor's rate. To hide the steps, every world and view matrix
// the game hands the renderer is remembered per object for the last two
// simulation steps, and the frame is drawn with a blend between them by how
// far the frame is past the last step. That draws the race one step behind,
// in exchange for motion at the full frame rate.
//
// Objects are told apart by the address the game passes the matrix from and
// how many times that address was used earlier in the frame. Pairs that don't
// look like the same object a step apart (a big jump, a big turn, or a
// spinning wheel) are drawn as they are.

#include "smooth.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include "framerate.h"
#include "patch.h"
#include "config.h"

namespace {

struct Rec {
  float prev[16];
  float cur[16];
  unsigned prev_tick = 0;
  unsigned tick = 0;
  bool has_prev = false;
};

std::unordered_map<uint64_t, Rec> g_recs;
std::unordered_map<uint64_t, uint32_t> g_uses;  // draws so far this frame per source
unsigned g_frames = 0;
unsigned g_total = 0, g_blended = 0;
unsigned g_race_frames = 0;  // frames drawn while smoothing could run
bool g_trace = false;          // SmoothLog: tracing this frame  // matrices seen / blended, for the log

bool Affine(const float* m) {
  return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && std::fabs(m[15] - 1.0f) < 1e-4f;
}

float Len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Blend two affine matrices: translation linearly, the three axes linearly and
// then made square again with their blended lengths. False when the two are
// too far apart to be one object moving for one step.
bool Blend(const float* a, const float* b, float t, float* out) {
  if (!Affine(a) || !Affine(b)) return false;
  float axes[3][3];
  float lens[3];
  for (int r = 0; r < 3; ++r) {
    const float la = Len3(a + r * 4), lb = Len3(b + r * 4);
    if (la < 1e-6f || lb < 1e-6f) return false;
    float dot = 0.0f;
    for (int c = 0; c < 3; ++c) dot += a[r * 4 + c] * b[r * 4 + c];
    if (dot / (la * lb) < 0.8f) return false;  // turned more than ~37 degrees
    if (std::fabs(la - lb) > 0.25f * (la > lb ? la : lb)) return false;
    lens[r] = la + (lb - la) * t;
    for (int c = 0; c < 3; ++c) axes[r][c] = a[r * 4 + c] + (b[r * 4 + c] - a[r * 4 + c]) * t;
  }
  // Translation jump check, relative to the object's size so it works for any unit.
  float d[3];
  for (int c = 0; c < 3; ++c) d[c] = b[12 + c] - a[12 + c];
  const float jump = Len3(d);
  const float scale = (lens[0] + lens[1] + lens[2]) / 3.0f;
  if (jump > 200.0f * scale) return false;
  // Gram-Schmidt on the blended axes, keeping the first axis' direction.
  float* x = axes[0];
  float* y = axes[1];
  float* z = axes[2];
  auto norm = [](float* v) {
    const float l = Len3(v);
    if (l < 1e-6f) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
  };
  auto dot3 = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
  if (!norm(x)) return false;
  float k = dot3(y, x);
  for (int c = 0; c < 3; ++c) y[c] -= k * x[c];
  if (!norm(y)) return false;
  k = dot3(z, x);
  const float k2 = dot3(z, y);
  for (int c = 0; c < 3; ++c) z[c] -= k * x[c] + k2 * y[c];
  if (!norm(z)) return false;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) out[r * 4 + c] = axes[r][c] * lens[r];
    out[r * 4 + 3] = 0.0f;
  }
  for (int c = 0; c < 3; ++c) out[12 + c] = a[12 + c] + d[c] * t;
  out[15] = 1.0f;
  return true;
}

}  // namespace

void SmoothTransform(uint32_t type, const float* in, const void* src, float* out) {
  std::memcpy(out, in, 16 * sizeof(float));
  unsigned tick;
  float alpha;
  if (!SimDrawPoint(&tick, &alpha)) return;
  const uint64_t source = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src)) << 8) | (type & 0xff);
  const uint32_t use = g_uses[source]++;
  const uint64_t key = source ^ (static_cast<uint64_t>(use) << 40);
  Rec& r = g_recs[key];
  if (r.tick != tick) {
    if (r.tick != 0) {
      std::memcpy(r.prev, r.cur, sizeof(r.prev));
      r.prev_tick = r.tick;
      r.has_prev = true;
    }
    r.tick = tick;
  }
  std::memcpy(r.cur, in, sizeof(r.cur));
  ++g_total;
  if (!r.has_prev || r.prev_tick + 1 != tick) return;
  float blended[16];
  if (Blend(r.prev, r.cur, alpha, blended)) {
    std::memcpy(out, blended, sizeof(blended));
    ++g_blended;
  }
  if (g_trace) {
    patch::Log("trace: set %s src=%p use=%u tick=%u a=%.3f in=(%.2f %.2f %.2f) out=(%.2f %.2f %.2f) "
               "prev=(%.2f %.2f %.2f) prevtick=%u",
               type == 1 ? "world" : "view", src, use, tick, alpha, in[12], in[13], in[14], out[12],
               out[13], out[14], r.prev[12], r.prev[13], r.prev[14], r.prev_tick);
  }
}

void SmoothNoteDraw(const char* what, bool tl, unsigned verts) {
  if (g_trace) patch::Log("trace: %s tl=%d verts=%u", what, tl ? 1 : 0, verts);
}

void SmoothFrameEnd() {
  g_uses.clear();
  {
    unsigned tick;
    float alpha;
    if (SimDrawPoint(&tick, &alpha)) {
      ++g_race_frames;
      // Trace 6 frames, 5 seconds or so into the race.
      g_trace = GetConfig().smooth_log && g_race_frames >= 600 && g_race_frames < 606;
      if (g_trace) patch::Log("trace: ---- frame %u, tick %u, a=%.3f", g_race_frames, tick, alpha);
    } else {
      g_trace = false;
    }
  }
  if (++g_frames % 600 == 0) {
    unsigned tick;
    float alpha;
    const bool on = SimDrawPoint(&tick, &alpha);
    if (on && g_total) {
      patch::Log("smoothing: %u of %u matrices blended over 600 frames, %u objects tracked",
                 g_blended, g_total, static_cast<unsigned>(g_recs.size()));
    }
    g_total = g_blended = 0;
    for (auto it = g_recs.begin(); it != g_recs.end();) {
      if (!on || it->second.tick + 30 < tick) it = g_recs.erase(it);
      else ++it;
    }
  }
}
