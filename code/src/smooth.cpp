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
#include <vector>

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

// Largest distance a vertex (or the camera) may move in one step and still be
// treated as the same thing moving (the game's units are about a foot; ~600 mph).
constexpr float kMaxStepMove = 30.0f;

std::unordered_map<uint64_t, Rec> g_recs;
std::unordered_map<uint64_t, uint32_t> g_uses;  // draws so far this frame per source
unsigned g_frames = 0;
unsigned g_total = 0, g_blended = 0;
unsigned g_race_frames = 0;  // frames drawn while smoothing could run
bool g_trace = false;          // SmoothLog: tracing this frame
bool g_trace_view = false;     // SmoothLog=2: tracing the camera this frame  // matrices seen / blended, for the log

bool Affine(const float* m) {
  return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && std::fabs(m[15] - 1.0f) < 1e-4f;
}

float Len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// Blend two affine matrices: translation linearly, the three axes linearly and
// then made square again with their blended lengths. False when the two are
// too far apart to be one object moving for one step.
bool Blend(const float* a, const float* b, float t, float* out, bool check_jump = true) {
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
  if (check_jump && jump > 200.0f * scale) return false;
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

// Camera position of a view matrix (row vectors: view = world * R + t, so the
// camera sits at -t * R^T for a rotation R; scale is divided out).
void ViewEye(const float* m, float* eye) {
  for (int r = 0; r < 3; ++r) {
    const float l2 = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
    eye[r] = 0.0f;
    for (int c = 0; c < 3; ++c) eye[r] -= m[12 + c] * m[r * 4 + c];
    if (l2 > 1e-12f) eye[r] /= l2;
  }
}

// Blend two view matrices: the camera's turn as in Blend, and its position
// along a straight line (blending the raw translation instead would swing
// the camera sideways while it turns).
bool BlendView(const float* a, const float* b, float t, float* out) {
  float ea[3], eb[3];
  ViewEye(a, ea);
  ViewEye(b, eb);
  // A view's translation swings by hundreds of units when the camera only
  // turns a little far from the world's origin, so check the camera's own
  // move instead.
  const float dx = eb[0] - ea[0], dy = eb[1] - ea[1], dz = eb[2] - ea[2];
  if (dx * dx + dy * dy + dz * dz > kMaxStepMove * kMaxStepMove) return false;
  if (!Blend(a, b, t, out, false)) return false;
  float eye[3];
  for (int c = 0; c < 3; ++c) eye[c] = ea[c] + (eb[c] - ea[c]) * t;
  for (int c = 0; c < 3; ++c) {
    out[12 + c] = -(eye[0] * out[0 * 4 + c] + eye[1] * out[1 * 4 + c] + eye[2] * out[2 * 4 + c]);
  }
  return true;
}

struct VertRec {
  std::vector<float> prev, cur;  // positions, 3 floats each
  unsigned prev_tick = 0, tick = 0;
  bool has_prev = false;
};
std::unordered_map<uint64_t, VertRec> g_vrecs;
std::unordered_map<uint64_t, uint32_t> g_vuses;
std::vector<uint8_t> g_vout;
unsigned g_vtotal = 0, g_vblended = 0;

// Largest distance a vertex may move apart from the object's average move in
// one step (turning, leaning, legs and arms) before the pair is taken to be
// two different objects.
constexpr float kMaxBend = 4.0f;
// Up to this, each vertex is blended on its own; beyond, the shape moves as a whole.
constexpr float kMaxPointBlend = 0.5f;

}  // namespace

const uint8_t* SmoothVertices(const uint8_t* verts, uint32_t stride, uint32_t count,
                              const void* idx) {
  unsigned tick;
  float alpha;
  if (!verts || stride < 12 || !count || !SimDrawPoint(&tick, &alpha)) return verts;
  // Same vertex memory, same index list, same size: same object (counted per frame,
  // since the game may reuse one scratch buffer for several objects).
  uint64_t source = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(verts));
  source = source * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(idx));
  source = source * 0x9E3779B97F4A7C15ull ^ count;
  const uint32_t use = g_vuses[source]++;
  const uint64_t key = source * 0x9E3779B97F4A7C15ull ^ use;
  VertRec& r = g_vrecs[key];
  if (r.tick != tick) {
    if (r.tick != 0 && r.cur.size() == static_cast<size_t>(count) * 3) {
      r.prev.swap(r.cur);
      r.prev_tick = r.tick;
      r.has_prev = true;
    } else {
      r.has_prev = false;
    }
    r.tick = tick;
  }
  r.cur.resize(static_cast<size_t>(count) * 3);
  for (uint32_t i = 0; i < count; ++i) std::memcpy(&r.cur[i * 3], verts + static_cast<size_t>(i) * stride, 12);
  ++g_vtotal;
  if (!r.has_prev || r.prev_tick + 1 != tick || r.prev.size() != r.cur.size()) return verts;
  // Check the whole object moved like one thing: no vertex jumped far, and
  // every vertex moved close to the object's average move (a different
  // object that happens to land in the same slot moves every vertex
  // differently, and blending it shows up as flickering triangles).
  double mean[3] = {0, 0, 0};
  for (size_t i = 0; i < r.cur.size(); i += 3) {
    const float dx = r.cur[i] - r.prev[i], dy = r.cur[i + 1] - r.prev[i + 1], dz = r.cur[i + 2] - r.prev[i + 2];
    if (dx * dx + dy * dy + dz * dz > kMaxStepMove * kMaxStepMove) return verts;
    mean[0] += dx; mean[1] += dy; mean[2] += dz;
  }
  for (double& m : mean) m /= count;
  double bend = 0.0;  // largest squared distance from the average move
  for (size_t i = 0; i < r.cur.size(); i += 3) {
    const double dx = r.cur[i] - r.prev[i] - mean[0], dy = r.cur[i + 1] - r.prev[i + 1] - mean[1],
                 dz = r.cur[i + 2] - r.prev[i + 2] - mean[2];
    const double d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > bend) bend = d2;
  }
  if (bend > kMaxBend * kMaxBend) return verts;
  g_vout.assign(verts, verts + static_cast<size_t>(count) * stride);
  // A shape that changes a lot in one step (a spinning wheel, a shadow cast
  // on bumpy ground) can't be blended point by point without squashing it:
  // draw its current shape, moved back along its average move instead.
  const bool whole = bend > kMaxPointBlend * kMaxPointBlend;
  const float back = 1.0f - alpha;
  for (uint32_t i = 0; i < count; ++i) {
    float p[3];
    for (int c = 0; c < 3; ++c) {
      p[c] = whole ? r.cur[i * 3 + c] - static_cast<float>(mean[c]) * back
                   : r.prev[i * 3 + c] + (r.cur[i * 3 + c] - r.prev[i * 3 + c]) * alpha;
    }
    std::memcpy(g_vout.data() + static_cast<size_t>(i) * stride, p, 12);
  }
  ++g_vblended;
  return g_vout.data();
}

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
  const bool ok = type == 2 ? BlendView(r.prev, r.cur, alpha, blended)
                            : Blend(r.prev, r.cur, alpha, blended);
  if (ok) {
    std::memcpy(out, blended, sizeof(blended));
    ++g_blended;
  }
  if (g_trace_view && type == 2) {
    patch::Log("view: f=%u tick=%u a=%.3f use=%u in=(%.3f %.3f %.3f | %.2f %.2f %.2f) out=(%.3f %.3f %.3f | %.2f %.2f %.2f)",
               g_race_frames, tick, alpha, use, in[0], in[1], in[2], in[12], in[13], in[14], out[0], out[1],
               out[2], out[12], out[13], out[14]);
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
  g_vuses.clear();
  {
    unsigned tick;
    float alpha;
    if (SimDrawPoint(&tick, &alpha)) {
      ++g_race_frames;
      // Trace 6 frames, 5 seconds or so into the race.
      const int mode = GetConfig().smooth_log;
      g_trace = mode == 1 && g_race_frames >= 600 && g_race_frames < 606;
      g_trace_view = mode == 2 && g_race_frames >= 600 && g_race_frames < 720;
      if (g_trace) patch::Log("trace: ---- frame %u, tick %u, a=%.3f", g_race_frames, tick, alpha);
    } else {
      g_trace = g_trace_view = false;
    }
  }
  if (++g_frames % 600 == 0) {
    unsigned tick;
    float alpha;
    const bool on = SimDrawPoint(&tick, &alpha);
    if (on && g_total) {
      patch::Log("smoothing: %u of %u matrices and %u of %u vertex draws blended over 600 frames, "
                 "%u + %u tracked",
                 g_blended, g_total, g_vblended, g_vtotal, static_cast<unsigned>(g_recs.size()),
                 static_cast<unsigned>(g_vrecs.size()));
    }
    g_total = g_blended = g_vtotal = g_vblended = 0;
    for (auto it = g_recs.begin(); it != g_recs.end();) {
      if (!on || it->second.tick + 30 < tick) it = g_recs.erase(it);
      else ++it;
    }
    for (auto it = g_vrecs.begin(); it != g_vrecs.end();) {
      if (!on || it->second.tick + 30 < tick) it = g_vrecs.erase(it);
      else ++it;
    }
  }
}
