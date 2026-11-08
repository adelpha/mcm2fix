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

#include <windows.h>

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
struct TextRec {
  int prev[2] = {}, cur[2] = {};
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uint64_t, TextRec> g_trecs;
std::unordered_map<uint64_t, uint32_t> g_tuses;
using TextOutFn = BOOL(WINAPI*)(HDC, int, int, LPCSTR, int);
TextOutFn g_text_out = nullptr;

// Names over riders are drawn with GDI at the spot the game projected for
// this step. Each piece of text is followed by its string (and how many
// times it was drawn so far this frame) and moved between its last two spots.
BOOL WINAPI SmoothTextOut(HDC dc, int x, int y, LPCSTR text, int len) {
  unsigned tick;
  float alpha;
  if (text && len > 0 && SimDrawPoint(&tick, &alpha)) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < len; ++i) h = (h ^ static_cast<uint8_t>(text[i])) * 1099511628211ull;
    const uint32_t use = g_tuses[h]++;
    TextRec& r = g_trecs[h * 0x9E3779B97F4A7C15ull ^ use];
    if (r.tick != tick) {
      r.prev[0] = r.cur[0];
      r.prev[1] = r.cur[1];
      r.prev_tick = r.tick;
      r.tick = tick;
    }
    r.cur[0] = x;
    r.cur[1] = y;
    const int dx = r.cur[0] - r.prev[0], dy = r.cur[1] - r.prev[1];
    if (r.prev_tick != 0 && r.prev_tick + 1 == tick && dx * dx + dy * dy < 400 * 400) {
      x = r.prev[0] + static_cast<int>(std::lround(dx * alpha));
      y = r.prev[1] + static_cast<int>(std::lround(dy * alpha));
    }
  }
  return g_text_out(dc, x, y, text, len);
}

struct CentreRec {
  float prev[3] = {}, cur[3] = {};
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uint64_t, CentreRec> g_crecs;
std::unordered_map<uint64_t, uint32_t> g_luses;

// Rotation (unit quaternion w,x,y,z) that best turns the points `a` onto
// `b` about their centres (Horn's method: the top eigenvector of a 4x4
// matrix, found by power iteration).
void FitRotation(const float* a, const float* b, uint32_t n, float* q) {
  double ca[3] = {0, 0, 0}, cb[3] = {0, 0, 0};
  for (uint32_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c) {
      ca[c] += a[i * 3 + c];
      cb[c] += b[i * 3 + c];
    }
  for (int c = 0; c < 3; ++c) {
    ca[c] /= n;
    cb[c] /= n;
  }
  double S[3][3] = {};
  for (uint32_t i = 0; i < n; ++i)
    for (int x = 0; x < 3; ++x)
      for (int y = 0; y < 3; ++y) S[x][y] += (a[i * 3 + x] - ca[x]) * (b[i * 3 + y] - cb[y]);
  const double N[4][4] = {
      {S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
      {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
      {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
      {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
  double shift = 0.0;  // make every eigenvalue positive so the largest wins
  for (auto& row : N)
    for (double v : row) shift += std::fabs(v);
  double v[4] = {1, 0, 0, 0};
  for (int it = 0; it < 60; ++it) {
    double w[4];
    for (int r = 0; r < 4; ++r) {
      w[r] = shift * v[r];
      for (int c = 0; c < 4; ++c) w[r] += N[r][c] * v[c];
    }
    double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3]);
    if (l < 1e-30) break;
    for (int r = 0; r < 4; ++r) v[r] = w[r] / l;
  }
  if (v[0] < 0)
    for (double& x : v) x = -x;
  for (int r = 0; r < 4; ++r) q[r] = static_cast<float>(v[r]);
}

// The rotation that undoes the fraction `f` of q.
void PartialInverse(const float* q, float f, float* out) {
  const float s = std::sqrt(q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (s < 1e-7f) {
    out[0] = 1.0f;
    out[1] = out[2] = out[3] = 0.0f;
    return;
  }
  const float angle = 2.0f * std::atan2(s, q[0]);
  const float half = -angle * f * 0.5f;
  const float k = std::sin(half) / s;
  out[0] = std::cos(half);
  out[1] = q[1] * k;
  out[2] = q[2] * k;
  out[3] = q[3] * k;
}

void Rotate(const float* q, const float* v, float* o) {
  // o = v + 2w(u x v) + 2 u x (u x v)
  const float ux = q[1], uy = q[2], uz = q[3], w = q[0];
  const float tx = 2.0f * (uy * v[2] - uz * v[1]);
  const float ty = 2.0f * (uz * v[0] - ux * v[2]);
  const float tz = 2.0f * (ux * v[1] - uy * v[0]);
  o[0] = v[0] + w * tx + (uy * tz - uz * ty);
  o[1] = v[1] + w * ty + (uz * tx - ux * tz);
  o[2] = v[2] + w * tz + (ux * ty - uy * tx);
}
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
  // Centre of this draw, also kept per slot regardless of size: a shadow cast
  // on bumpy ground changes its number of points every step, so for those
  // only the centre can be followed.
  double centre[3] = {0, 0, 0};
  for (size_t i = 0; i < r.cur.size(); i += 3)
    for (int c = 0; c < 3; ++c) centre[c] += r.cur[i + c];
  for (double& c : centre) c /= count;
  uint64_t loose = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(verts));
  loose = loose * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(idx));
  const uint32_t loose_use = g_luses[loose]++;
  CentreRec& cr = g_crecs[loose * 0x9E3779B97F4A7C15ull ^ loose_use];
  if (cr.tick != tick) {
    std::memcpy(cr.prev, cr.cur, sizeof(cr.prev));
    cr.prev_tick = cr.tick;
    cr.tick = tick;
  }
  for (int c = 0; c < 3; ++c) cr.cur[c] = static_cast<float>(centre[c]);
  const float back = 1.0f - alpha;

  if (!r.has_prev || r.prev_tick + 1 != tick || r.prev.size() != r.cur.size()) {
    // Following only the centre also caught batches of ground and trees whose
    // size changes as pieces come into view, which moved the world around.
    // Kept for a future, narrower match (shadows only); off for now.
    constexpr bool kFollowCentres = false;
    if (!kFollowCentres || cr.prev_tick == 0 || cr.prev_tick + 1 != tick) return verts;
    float d[3];
    for (int c = 0; c < 3; ++c) d[c] = cr.cur[c] - cr.prev[c];
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > kMaxStepMove * kMaxStepMove) return verts;
    g_vout.assign(verts, verts + static_cast<size_t>(count) * stride);
    for (uint32_t i = 0; i < count; ++i) {
      float p[3];
      for (int c = 0; c < 3; ++c) p[c] = r.cur[i * 3 + c] - d[c] * back;
      std::memcpy(g_vout.data() + static_cast<size_t>(i) * stride, p, 12);
    }
    ++g_vblended;
    return g_vout.data();
  }
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
  if (bend <= kMaxPointBlend * kMaxPointBlend) {
    for (uint32_t i = 0; i < count; ++i) {
      float p[3];
      for (int c = 0; c < 3; ++c) p[c] = r.prev[i * 3 + c] + (r.cur[i * 3 + c] - r.prev[i * 3 + c]) * alpha;
      std::memcpy(g_vout.data() + static_cast<size_t>(i) * stride, p, 12);
    }
  } else {
    // A shape that turns or changes a lot in one step (a wheel spinning while
    // the bike turns) can't be blended point by point without squashing it:
    // find how it turned as a whole, and draw its current shape turned and
    // moved back by the part of the step not yet shown.
    float q[4];
    FitRotation(r.prev.data(), r.cur.data(), count, q);
    float qb[4];
    PartialInverse(q, back, qb);
    float cc[3], cp[3];
    for (int c = 0; c < 3; ++c) {
      cc[c] = static_cast<float>(centre[c]);
      cp[c] = static_cast<float>(centre[c] - mean[c]);
    }
    for (uint32_t i = 0; i < count; ++i) {
      float v[3], o[3];
      for (int c = 0; c < 3; ++c) v[c] = r.cur[i * 3 + c] - cc[c];
      Rotate(qb, v, o);
      float p[3];
      for (int c = 0; c < 3; ++c) p[c] = o[c] + cp[c] + (cc[c] - cp[c]) * alpha;
      std::memcpy(g_vout.data() + static_cast<size_t>(i) * stride, p, 12);
    }
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
  g_luses.clear();
  g_tuses.clear();
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
    for (auto it = g_crecs.begin(); it != g_crecs.end();) {
      if (!on || it->second.tick + 30 < tick) it = g_crecs.erase(it);
      else ++it;
    }
    for (auto it = g_trecs.begin(); it != g_trecs.end();) {
      if (!on || it->second.tick + 30 < tick) it = g_trecs.erase(it);
      else ++it;
    }
    for (auto it = g_vrecs.begin(); it != g_vrecs.end();) {
      if (!on || it->second.tick + 30 < tick) it = g_vrecs.erase(it);
      else ++it;
    }
  }
}

void InstallTextSmoothing() {
  constexpr uintptr_t kIatTextOutA = 0x555088;
  void* old = patch::HookIat(kIatTextOutA, reinterpret_cast<void*>(&SmoothTextOut));
  if (old) g_text_out = reinterpret_cast<TextOutFn>(old);
  patch::Log("smoothing: text over riders %s", old ? "follows the blend" : "not hooked");
}
