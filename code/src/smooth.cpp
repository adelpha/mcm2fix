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
bool g_bypass = false;  // drawing something already placed between steps (shadow camera)
unsigned g_shadow_blends = 0;  // shadows rebuilt from blended riders
unsigned g_shadow_why[6] = {};  // calls, no meshes, no prev, moved too far, off, ok
unsigned g_total = 0, g_blended = 0;
unsigned g_race_frames = 0;  // frames drawn while smoothing could run
bool g_trace = false;          // SmoothLog: tracing this frame
float g_arrow_pos[3] = {};
unsigned g_arrow_frames = 0;  // ArrowLog: frames left to trace
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
  if (!verts || stride < 12 || !count || SmoothPartsOwn(verts) || !SimDrawPoint(&tick, &alpha)) return verts;
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

// Projection: the camera's zoom (field of view) changes once per step, e.g.
// the thrill cam zooming as the rider flies past. Blend the scale terms of
// perspective projections between the last two steps, matched by the order
// the game sets them in a frame.
struct ProjRec {
  float prev[4] = {}, cur[4] = {};  // m[0], m[5], m[8], m[9]
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uint32_t, ProjRec> g_projs;
uint32_t g_proj_use = 0;
unsigned g_proj_why[4] = {};  // seen, blended, no previous, jumped
bool g_proj_sample = false;

void SmoothArrowTrace(const float pos[3], unsigned frames) {
  std::memcpy(g_arrow_pos, pos, sizeof(g_arrow_pos));
  if (frames > g_arrow_frames) g_arrow_frames = frames;
}

bool SmoothArrowTracing(const float* m) {
  if (!g_arrow_frames) return false;
  const float dx = m[12] - g_arrow_pos[0], dy = m[13] - g_arrow_pos[1], dz = m[14] - g_arrow_pos[2];
  return dx * dx + dy * dy + dz * dz < 25.0f;
}

void SmoothProjection(float* m) {
  unsigned tick;
  float alpha;
  if (m[11] == 0.0f || !SimDrawPoint(&tick, &alpha)) return;  // not a perspective one, or not racing
  ++g_proj_why[0];
  ProjRec& r = g_projs[g_proj_use++];
  if (r.tick != tick) {
    std::memcpy(r.prev, r.cur, sizeof(r.prev));
    r.prev_tick = r.tick;
    r.tick = tick;
  }
  r.cur[0] = m[0]; r.cur[1] = m[5]; r.cur[2] = m[8]; r.cur[3] = m[9];
  if (g_arrow_frames)
    patch::Log("arrow trace: projection tick %u a=%.3f m00 prev %.4f cur %.4f (prev tick %u)", tick, alpha, r.prev[0],
               r.cur[0], r.prev_tick);
  if (r.prev_tick == 0 || r.prev_tick + 1 != tick) { ++g_proj_why[2]; return; }
  if (r.prev[0] == 0.0f || r.prev[1] == 0.0f) { ++g_proj_why[2]; return; }
  const float k0 = r.cur[0] / r.prev[0], k1 = r.cur[1] / r.prev[1];
  if (k0 < 0.5f || k0 > 2.0f || k1 < 0.5f || k1 > 2.0f) { ++g_proj_why[3]; return; }  // a cut, not a zoom
  if (r.prev[0] == r.cur[0] && r.prev[1] == r.cur[1]) return;
  float out[4];
  for (int i = 0; i < 4; ++i) out[i] = r.prev[i] + (r.cur[i] - r.prev[i]) * alpha;
  if (!g_proj_sample) {
    g_proj_sample = true;
    patch::Log("smoothing: projection zoom %.4f -> %.4f drawn at a=%.2f as %.4f", r.prev[0], r.cur[0], alpha, out[0]);
  }
  m[0] = out[0]; m[5] = out[1]; m[8] = out[2]; m[9] = out[3];
  ++g_proj_why[1];
}

void SmoothTransform(uint32_t type, const float* in, const void* src, float* out) {
  std::memcpy(out, in, 16 * sizeof(float));
  unsigned tick;
  float alpha;
  if (g_bypass || !SimDrawPoint(&tick, &alpha)) return;
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
  if (g_trace_view && type == 2 && g_arrow_frames) {
    float ei[3], eo[3];
    ViewEye(in, ei);
    ViewEye(out, eo);
    patch::Log("arrow trace: view tick %u a=%.3f use %u eye now (%.2f %.2f %.2f) drawn (%.2f %.2f %.2f)%s", tick, alpha,
               use, ei[0], ei[1], ei[2], eo[0], eo[1], eo[2], ok ? "" : " NOT blended");
  } else if (g_trace_view && type == 2) {
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

void LogShadowDetail();
void ProjectedShadowFrameEnd();

void SmoothFrameEnd() {
  ProjectedShadowFrameEnd();
  PartsFrameEnd(g_frames % 600 == 599);
  g_uses.clear();
  g_proj_use = 0;
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
      g_trace_view = (mode == 2 && g_race_frames >= 600 && g_race_frames < 720) || g_arrow_frames > 0;
      if (g_arrow_frames > 0) {
        --g_arrow_frames;
        patch::Log("arrow trace: ---- frame %u, tick %u, a=%.3f", g_race_frames, tick, alpha);
      }
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
    if (on) {
      patch::Log("smoothing: %u shadow draws rebuilt (calls %u, no meshes %u, no previous %u, jumped %u, off %u)",
                 g_shadow_blends, g_shadow_why[0], g_shadow_why[1], g_shadow_why[2], g_shadow_why[3],
                 g_shadow_why[4]);
      LogShadowDetail();
    }
    if (on && g_proj_why[0]) {
      patch::Log("smoothing: projections: %u seen, %u zoom blended; skipped: no previous %u, cut %u",
                 g_proj_why[0], g_proj_why[1], g_proj_why[2], g_proj_why[3]);
    }
    for (unsigned& w : g_proj_why) w = 0;
    g_proj_sample = false;
    if (!on) g_projs.clear();
    for (unsigned& w : g_shadow_why) w = 0;
    g_total = g_blended = g_vtotal = g_vblended = 0;
    g_shadow_blends = 0;
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

// ---------------------------------------------------------------- shadows
//
// Rider and bike shadows (D3DIMSoultreeShadow, vtable 0x556624) are built
// by projecting the rider's meshes onto the ground (0x447ec0) and drawn by
// its draw method (0x4484c0). Blending the finished shadow doesn't work
// (its shape and size change every step), so at draw time we put the
// blended mesh positions in place, rebuild the shadow from them, draw it,
// and put the game's positions back.

namespace {

using ShadowFn = int(__thiscall*)(void*);
const ShadowFn kShadowBuild = reinterpret_cast<ShadowFn>(0x447EC0);
const ShadowFn kShadowDraw = reinterpret_cast<ShadowFn>(0x4484C0);

struct ShadowRec {
  std::vector<float*> arrays;  // each mesh's vertex positions (stride 32 bytes)
  std::vector<uint32_t> counts;
  std::vector<float> prev, cur;
  float prev_m[16] = {}, cur_m[16] = {};  // the projection at [obj+0x30]+0xd8
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uintptr_t, ShadowRec> g_shadows;

// The meshes the shadow is built from: list at [mesh+0x28c] picked by the
// detail level at [mesh+0x27c]; each entry {count, items}, items 0x38 bytes
// with the vertex count at +0x8 and the vertices at +0x10.
bool ShadowMeshes(uintptr_t obj, std::vector<float*>& arrays, std::vector<uint32_t>& counts) {
  arrays.clear();
  counts.clear();
  const uintptr_t mesh = *reinterpret_cast<uintptr_t*>(obj + 0x2c);
  if (!mesh) return false;
  const int level = *reinterpret_cast<int*>(mesh + 0x27c);
  const uintptr_t lists = *reinterpret_cast<uintptr_t*>(mesh + 0x28c);
  if (!lists || level < 0 || level > 16) return false;
  const int n = *reinterpret_cast<int*>(lists + level * 8);
  const uintptr_t items = *reinterpret_cast<uintptr_t*>(lists + level * 8 + 4);
  if (n <= 0 || n > 256 || !items) return false;
  for (int i = 0; i < n; ++i) {
    const uintptr_t item = items + static_cast<uintptr_t>(i) * 0x38;
    const int count = *reinterpret_cast<int*>(item + 0x8);
    auto* verts = *reinterpret_cast<float**>(item + 0x10);
    if (count <= 0 || count > 65536 || !verts) return false;
    arrays.push_back(verts);
    counts.push_back(static_cast<uint32_t>(count));
  }
  return true;
}

int __fastcall SmoothShadowDraw(void* self, void*) {
  const uintptr_t obj = reinterpret_cast<uintptr_t>(self);
  unsigned tick;
  float alpha;
  ++g_shadow_why[0];
  if (!SimDrawPoint(&tick, &alpha)) { ++g_shadow_why[4]; return kShadowDraw(self); }
  ShadowRec& r = g_shadows[obj];
  std::vector<float*> arrays;
  std::vector<uint32_t> counts;
  if (!ShadowMeshes(obj, arrays, counts)) { ++g_shadow_why[1]; return kShadowDraw(self); }
  size_t total = 0;
  for (uint32_t c : counts) total += c;
  if (r.tick != tick) {
    const bool same = arrays == r.arrays && counts == r.counts && r.cur.size() == total * 3;
    if (same) {
      r.prev.swap(r.cur);
      r.prev_tick = r.tick;
    } else {
      r.prev_tick = 0;
    }
    r.arrays = arrays;
    r.counts = counts;
    r.tick = tick;
    r.cur.resize(total * 3);
    std::memcpy(r.prev_m, r.cur_m, sizeof(r.prev_m));
    if (const uintptr_t proj = *reinterpret_cast<uintptr_t*>(obj + 0x30))
      std::memcpy(r.cur_m, reinterpret_cast<void*>(proj + 0xd8), sizeof(r.cur_m));
    size_t k = 0;
    for (size_t m = 0; m < arrays.size(); ++m)
      for (uint32_t i = 0; i < counts[m]; ++i, k += 3)
        std::memcpy(&r.cur[k], reinterpret_cast<uint8_t*>(arrays[m]) + i * 32, 12);
  }
  if (r.prev_tick == 0 || r.prev_tick + 1 != tick || arrays != r.arrays || r.prev.size() != r.cur.size()) {
    ++g_shadow_why[2];
    return kShadowDraw(self);
  }
  for (size_t k = 0; k < r.cur.size(); k += 3) {
    const float dx = r.cur[k] - r.prev[k], dy = r.cur[k + 1] - r.prev[k + 1], dz = r.cur[k + 2] - r.prev[k + 2];
    if (dx * dx + dy * dy + dz * dz > kMaxStepMove * kMaxStepMove) { ++g_shadow_why[3]; return kShadowDraw(self); }
  }
  // Blended positions in, rebuild, draw, game's positions back.
  size_t k = 0;
  for (size_t m = 0; m < arrays.size(); ++m)
    for (uint32_t i = 0; i < counts[m]; ++i, k += 3) {
      float p[3];
      for (int c = 0; c < 3; ++c) p[c] = r.prev[k + c] + (r.cur[k + c] - r.prev[k + c]) * alpha;
      std::memcpy(reinterpret_cast<uint8_t*>(arrays[m]) + i * 32, p, 12);
    }
  float* mat = nullptr;
  if (const uintptr_t proj = *reinterpret_cast<uintptr_t*>(obj + 0x30)) {
    mat = reinterpret_cast<float*>(proj + 0xd8);
    for (int c = 0; c < 16; ++c) mat[c] = r.prev_m[c] + (r.cur_m[c] - r.prev_m[c]) * alpha;
  }
  kShadowBuild(self);
  const int result = kShadowDraw(self);
  if (mat) std::memcpy(mat, r.cur_m, sizeof(r.cur_m));
  k = 0;
  for (size_t m = 0; m < arrays.size(); ++m)
    for (uint32_t i = 0; i < counts[m]; ++i, k += 3)
      std::memcpy(reinterpret_cast<uint8_t*>(arrays[m]) + i * 32, &r.cur[k], 12);
  ++g_shadow_blends;
  return result;
}

}  // namespace

// First shadow's meshes, to see what they hold.
void LogShadowDetail() {
      for (auto& kv : g_shadows) {
        const ShadowRec& r = kv.second;
        patch::Log("smoothing: shadow %08x: %u meshes, %u points, first (%.2f %.2f %.2f) was (%.2f %.2f %.2f)",
                   static_cast<unsigned>(kv.first), static_cast<unsigned>(r.arrays.size()),
                   static_cast<unsigned>(r.cur.size() / 3), r.cur.empty() ? 0.f : r.cur[0],
                   r.cur.empty() ? 0.f : r.cur[1], r.cur.empty() ? 0.f : r.cur[2],
                   r.prev.empty() ? 0.f : r.prev[0], r.prev.empty() ? 0.f : r.prev[1],
                   r.prev.empty() ? 0.f : r.prev[2]);
        break;
      }
}

void InstallShadowSmoothing() {
  constexpr uintptr_t kVtable = 0x556624;
  constexpr uintptr_t kSlot = kVtable + 0x38;
  if (*reinterpret_cast<uintptr_t*>(kSlot) != 0x4484C0) {
    patch::Log("smoothing: shadow draw slot differs, shadows not smoothed");
    return;
  }
  void* fn = reinterpret_cast<void*>(&SmoothShadowDraw);
  patch::Write(kSlot, &fn, sizeof(fn));
  patch::Log("smoothing: shadows rebuilt from blended riders every frame");
}

// ---------------------------------------------------------------- projected shadows
//
// The riders' shadows come from one ProjectedShadow object (vtable
// 0x55c848). Before the scene is drawn (slot 0x30, 0x4df7f0) it fits a
// shadow window around its casters (array [obj+0x2c], count [obj+0x30])
// using each caster's world matrix at +0xf8 (4x4, translation at +0x128),
// draws the casters into a texture from a fixed light camera, and gives the
// ground in the window texture coordinates; then (slot 0x34, 0x4e0c00) it
// draws that ground with the texture. The casters' vertices are already
// blended between steps, but their matrices are the step's, so the window
// jumps once per step. So around those two calls we put each caster's
// matrix between its last two steps, and put the game's back afterwards.

namespace {

using PsFn = int(__thiscall*)(void*);
const PsFn kPsPreDraw = reinterpret_cast<PsFn>(0x4DF7F0);
const PsFn kPsDraw = reinterpret_cast<PsFn>(0x4E0C00);
constexpr uintptr_t kCasterMatrix = 0xf8;

struct CasterRec {
  float prev[16] = {}, cur[16] = {};
  unsigned prev_tick = 0, tick = 0;
  bool written = false;  // our blend is in the caster right now
};
std::unordered_map<uintptr_t, CasterRec> g_casters;
std::vector<uintptr_t> g_casters_written;
// pre-draws, draws, off, casters seen, blended, no previous, jumped/turned, restored at frame end
unsigned g_ps_why[8] = {};
// texture re-renders: game asked, we forced, rendered something (+0x130), last period seen (+0x120)
unsigned g_ps_tex[4] = {};
bool g_ps_logged_sample = false;

// The shadow texture is drawn in software from each caster's mesh vertices,
// which the game keeps in world space and only moves once per step. Blend
// them in place for the texture render and put them back right after.
using GetMeshFn = int(__thiscall*)(void*, int, uintptr_t*, int*, uintptr_t*, int*, int, int);
const GetMeshFn kGetMesh = reinterpret_cast<GetMeshFn>(0x444390);
using MeshCountFn = int(__thiscall*)(void*, int);
const MeshCountFn kMeshCount = reinterpret_cast<MeshCountFn>(0x445FD0);
constexpr int kMeshStride = 32;
struct MeshRec {
  std::vector<float> prev, cur;  // xyz per vertex
  unsigned prev_tick = 0, tick = 0;
};
std::unordered_map<uintptr_t, MeshRec> g_meshes;
struct MeshWritten { uintptr_t verts; int count; };
std::vector<MeshWritten> g_meshes_written;
// meshes seen, blended, count changed, no previous, jumped, vertices blended, dirty (+0x18c) seen, sum of mean move
unsigned g_pm_why[7] = {};
double g_pm_move = 0.0;
bool g_pm_logged_sample = false;

void MeshRestore() {
  for (const MeshWritten& w : g_meshes_written) {
    auto it = g_meshes.find(w.verts);
    if (it == g_meshes.end() || static_cast<int>(it->second.cur.size()) != w.count * 3) continue;
    const float* src = it->second.cur.data();
    for (int i = 0; i < w.count; ++i) {
      float* v = reinterpret_cast<float*>(w.verts + static_cast<uintptr_t>(i) * kMeshStride);
      v[0] = src[i * 3]; v[1] = src[i * 3 + 1]; v[2] = src[i * 3 + 2];
    }
  }
  g_meshes_written.clear();
}

void BlendCasterMeshes(uintptr_t c, unsigned tick, float alpha) {
  void* caster = reinterpret_cast<void*>(c);
  if (*reinterpret_cast<int*>(c + 0x18c)) ++g_pm_why[6];
  const int meshes = kMeshCount(caster, -1);
  if (meshes <= 0 || meshes > 256) return;
  for (int j = 0; j < meshes; ++j) {
    uintptr_t verts = 0, idx = 0;
    int count = 0, nidx = 0;
    kGetMesh(caster, j, &verts, &count, &idx, &nidx, 0, -1);
    if (!verts || count <= 0 || count > 65536) continue;
    ++g_pm_why[0];
    MeshRec& r = g_meshes[verts];
    if (r.tick != tick) {
      r.prev.swap(r.cur);
      r.prev_tick = r.tick;
      r.tick = tick;
      r.cur.resize(static_cast<size_t>(count) * 3);
      for (int i = 0; i < count; ++i) {
        const float* v = reinterpret_cast<const float*>(verts + static_cast<uintptr_t>(i) * kMeshStride);
        r.cur[i * 3] = v[0]; r.cur[i * 3 + 1] = v[1]; r.cur[i * 3 + 2] = v[2];
      }
    }
    if (r.prev.size() != r.cur.size() || static_cast<int>(r.cur.size()) != count * 3) { ++g_pm_why[2]; continue; }
    if (r.prev_tick == 0 || r.prev_tick + 1 != tick) { ++g_pm_why[3]; continue; }
    double move = 0.0;
    for (int i = 0; i < count; ++i) {
      const float dx = r.cur[i * 3] - r.prev[i * 3], dy = r.cur[i * 3 + 1] - r.prev[i * 3 + 1],
                  dz = r.cur[i * 3 + 2] - r.prev[i * 3 + 2];
      move += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    move /= count;
    if (move > kMaxStepMove) { ++g_pm_why[4]; continue; }
    for (int i = 0; i < count; ++i) {
      float* v = reinterpret_cast<float*>(verts + static_cast<uintptr_t>(i) * kMeshStride);
      for (int k = 0; k < 3; ++k) v[k] = r.prev[i * 3 + k] + (r.cur[i * 3 + k] - r.prev[i * 3 + k]) * alpha;
    }
    g_meshes_written.push_back({verts, count});
    ++g_pm_why[1];
    g_pm_why[5] += count;
    g_pm_move += move;
    if (!g_pm_logged_sample) {
      g_pm_logged_sample = true;
      patch::Log("smoothing: shadow mesh %08x of caster %08x: %d vertices, first at (%.2f %.2f %.2f) was "
                 "(%.2f %.2f %.2f), mean move %.3f, drawn at a=%.2f",
                 static_cast<unsigned>(verts), static_cast<unsigned>(c), count, r.cur[0], r.cur[1], r.cur[2],
                 r.prev[0], r.prev[1], r.prev[2], move, alpha);
    }
  }
}
unsigned g_ps_casters_max = 0;
unsigned g_ps_dirtied = 0;  // casters we marked for a rebuild from blended parts (frames after a step)

void PsRestore() {
  for (uintptr_t c : g_casters_written) {
    CasterRec& r = g_casters[c];
    if (!r.written) continue;
    std::memcpy(reinterpret_cast<void*>(c + kCasterMatrix), r.cur, sizeof(r.cur));
    r.written = false;
  }
  g_casters_written.clear();
}

int __fastcall SmoothPsPreDraw(void* self, void*) {
  ++g_ps_why[0];
  const uintptr_t obj = reinterpret_cast<uintptr_t>(self);
  unsigned tick;
  float alpha;
  PsRestore();  // in case last frame's draw never came
  if (!SimDrawPoint(&tick, &alpha)) { ++g_ps_why[2]; return kPsPreDraw(self); }
  const auto* casters = *reinterpret_cast<uintptr_t**>(obj + 0x2c);
  const int n = *reinterpret_cast<int*>(obj + 0x30);
  if (!casters || n <= 0 || n > 64) { ++g_ps_why[2]; return kPsPreDraw(self); }
  if (static_cast<unsigned>(n) > g_ps_casters_max) g_ps_casters_max = n;
  int dirty_before[4] = {};  // for the SmoothLog=3 trace
  for (int i = 0; i < n && i < 4; ++i)
    if (casters[i]) dirty_before[i] = *reinterpret_cast<int*>(casters[i] + 0x18c);
  for (int i = 0; i < n; ++i) {
    const uintptr_t c = casters[i];
    if (!c) continue;
    ++g_ps_why[3];
    if (!SmoothPartsActive()) {
      BlendCasterMeshes(c, tick, alpha);
    } else {
      // The shadow texture is drawn from the caster's world-space vertices,
      // which GetMesh (0x444390) rebuilds only when the object is marked
      // dirty. Between steps our draw leaves it marked, so they are rebuilt
      // from blended parts; but on a frame right after a simulation step
      // the game has already rebuilt them at the new step's positions and
      // cleared the mark, while the shadow window is placed with the blend
      // (still near the previous step). The shadow then jumped ahead of the
      // rider for one frame in every step. Mark it, so they are rebuilt from
      // blended parts every frame.
      int& dirty = *reinterpret_cast<int*>(c + 0x18c);
      if (!dirty) {
        dirty = 1;
        ++g_ps_dirtied;
      }
    }
    CasterRec& r = g_casters[c];
    float* m = reinterpret_cast<float*>(c + kCasterMatrix);
    if (r.tick != tick) {
      std::memcpy(r.prev, r.cur, sizeof(r.prev));
      r.prev_tick = r.tick;
      r.tick = tick;
    }
    std::memcpy(r.cur, m, sizeof(r.cur));
    if (r.prev_tick == 0 || r.prev_tick + 1 != tick) { ++g_ps_why[5]; continue; }
    float out[16];
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = r.cur[12 + k] - r.prev[12 + k];
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > kMaxStepMove * kMaxStepMove ||
        !Blend(r.prev, r.cur, alpha, out)) {
      ++g_ps_why[6];
      continue;
    }
    std::memcpy(m, out, sizeof(out));
    r.written = true;
    g_casters_written.push_back(c);
    ++g_ps_why[4];
    if (!g_ps_logged_sample) {
      g_ps_logged_sample = true;
      patch::Log("smoothing: shadow caster %08x at (%.2f %.2f %.2f) was (%.2f %.2f %.2f), drawn at a=%.2f "
                 "(%.2f %.2f %.2f)",
                 static_cast<unsigned>(c), r.cur[12], r.cur[13], r.cur[14], r.prev[12], r.prev[13],
                 r.prev[14], alpha, out[12], out[13], out[14]);
    }
  }
  // The game only re-renders the shadow texture when its update (once per sim
  // step, every [+0x120] steps) sets +0x11c; otherwise the old texture from
  // the last step is drawn. Ask for a fresh render every frame so the shadow
  // shows the riders where they are drawn now.
  // SmoothLog=3: trace the shadow window and the first caster's mesh.
  const bool trace = GetConfig().smooth_log == 3 && g_race_frames >= 900 && g_race_frames < 2700;
  int* want = reinterpret_cast<int*>(obj + 0x11c);
  g_ps_tex[3] = *reinterpret_cast<unsigned*>(obj + 0x120);
  if (*want) ++g_ps_tex[0];
  else { *want = 1; ++g_ps_tex[1]; }
  const int result = kPsPreDraw(self);
  if (trace) {
    const float* ctr = reinterpret_cast<const float*>(obj + 0xcc);
    const float* ext = reinterpret_cast<const float*>(obj + 0xc0);
    patch::Log("shadow trace: frame %u tick %u a=%.3f window centre (%.3f %.3f %.3f) extent (%.3f %.3f %.3f) k %.5f "
               "rendered %d",
               g_race_frames, tick, alpha, ctr[0], ctr[1], ctr[2], ext[0], ext[1], ext[2],
               *reinterpret_cast<const float*>(obj + 0xbc), *reinterpret_cast<const int*>(obj + 0x130));
    for (int i = 0; i < n && i < 4; ++i) {
      const uintptr_t c = casters[i];
      if (!c) continue;
      const float* m = reinterpret_cast<const float*>(c + kCasterMatrix);
      const CasterRec& r = g_casters[c];
      uintptr_t verts = 0, idx = 0;
      int count = 0, nidx = 0;
      const int dirty_now = *reinterpret_cast<int*>(c + 0x18c);
      float v0[3] = {};
      if (!dirty_now) {  // reading the mesh would rebuild a dirty object; skip then
        kGetMesh(reinterpret_cast<void*>(c), 0, &verts, &count, &idx, &nidx, 0, -1);
        if (verts && count > 0) std::memcpy(v0, reinterpret_cast<const float*>(verts), sizeof(v0));
      }
      patch::Log("shadow trace:   caster %d %08x matrix (%.3f %.3f %.3f) step (%.3f %.3f %.3f) prev (%.3f %.3f %.3f)%s "
                 "dirty %d->%d mesh0 v0 (%.3f %.3f %.3f) v0-matrix (%.3f %.3f %.3f)",
                 i, static_cast<unsigned>(c), m[12], m[13], m[14], r.cur[12], r.cur[13], r.cur[14], r.prev[12],
                 r.prev[13], r.prev[14], r.written ? " blended" : " NOT blended", dirty_before[i], dirty_now, v0[0],
                 v0[1], v0[2], v0[0] - m[12], v0[1] - m[13], v0[2] - m[14]);
    }
  }
  MeshRestore();  // the scene draws read these too; give them back the step's values
  if (*reinterpret_cast<int*>(obj + 0x130)) ++g_ps_tex[2];
  return result;
}

int __fastcall SmoothPsDraw(void* self, void*) {
  ++g_ps_why[1];
  const int result = kPsDraw(self);
  PsRestore();
  return result;
}

bool PatchSlot(uintptr_t slot, uintptr_t expected, void* fn, const char* what) {
  if (*reinterpret_cast<uintptr_t*>(slot) != expected) {
    patch::Log("smoothing: %s slot at %08x holds %08x, not %08x; not hooked", what,
               static_cast<unsigned>(slot), *reinterpret_cast<unsigned*>(slot), static_cast<unsigned>(expected));
    return false;
  }
  patch::Write(slot, &fn, sizeof(fn));
  return true;
}

}  // namespace

void ProjectedShadowFrameEnd() {
  if (!g_casters_written.empty()) {
    ++g_ps_why[7];
    PsRestore();
  }
  if (g_frames % 600 == 599) {
    if (g_ps_why[0]) {
      patch::Log("smoothing: projected shadows: %u pre-draws, %u draws; casters seen %u (most at once %u), "
                 "blended %u; skipped: off %u, no previous %u, jumped or turned %u; restored at frame end %u; "
                 "%u casters tracked",
                 g_ps_why[0], g_ps_why[1], g_ps_why[3], g_ps_casters_max, g_ps_why[4], g_ps_why[2], g_ps_why[5],
                 g_ps_why[6], g_ps_why[7], static_cast<unsigned>(g_casters.size()));
    }
    if (g_ps_why[0]) {
      patch::Log("smoothing: shadow texture renders: game asked %u, forced %u, drew casters %u; period %u steps; "
                 "casters re-marked for a blended rebuild %u",
                 g_ps_tex[0], g_ps_tex[1], g_ps_tex[2], g_ps_tex[3], g_ps_dirtied);
      g_ps_dirtied = 0;
    }
    if (g_ps_why[0]) {
      patch::Log("smoothing: shadow meshes: %u seen, %u blended (%u vertices, mean move %.3f); skipped: count "
                 "changed %u, no previous %u, jumped %u; caster rebuilds pending %u; %u meshes tracked",
                 g_pm_why[0], g_pm_why[1], g_pm_why[5], g_pm_why[1] ? g_pm_move / g_pm_why[1] : 0.0, g_pm_why[2],
                 g_pm_why[3], g_pm_why[4], g_pm_why[6], static_cast<unsigned>(g_meshes.size()));
    }
    for (unsigned& w : g_pm_why) w = 0;
    g_pm_move = 0.0;
    g_pm_logged_sample = false;
    for (unsigned& w : g_ps_tex) w = 0;
    for (unsigned& w : g_ps_why) w = 0;
    g_ps_logged_sample = false;
    g_ps_casters_max = 0;
    unsigned tick;
    float alpha;
    if (!SimDrawPoint(&tick, &alpha)) { g_casters.clear(); g_meshes.clear(); }
  }
}

void InstallProjectedShadowSmoothing() {
  constexpr uintptr_t kVtable = 0x55C848;
  const bool a = PatchSlot(kVtable + 0x30, 0x4DF7F0, reinterpret_cast<void*>(&SmoothPsPreDraw), "projected shadow pre-draw");
  const bool b = PatchSlot(kVtable + 0x34, 0x4E0C00, reinterpret_cast<void*>(&SmoothPsDraw), "projected shadow draw");
  patch::Log("smoothing: projected shadows %s", a && b ? "re-rendered every frame from casters placed between steps" : "NOT hooked");
}
