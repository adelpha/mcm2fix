// Run-time choice between the plain and SSE2 kernels (see simd.h), start-up
// self test and timing, and SimdLog statistics.

#include "simd.h"

#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "patch.h"
#include "simd_impl.h"

namespace simd {
namespace {

bool g_init = false;
bool g_sse2 = false;
bool g_log = false;

// ---- CPU ----------------------------------------------------------------------
struct Cpu {
  char vendor[13] = {};
  char brand[49] = {};
  bool mmx = false, sse = false, sse2 = false, sse3 = false, ssse3 = false, sse41 = false, sse42 = false,
       avx = false, avx2 = false, now3d = false;
};

void CpuId(unsigned leaf, unsigned sub, unsigned r[4]) {
#if defined(_MSC_VER)
  int v[4];
  __cpuidex(v, static_cast<int>(leaf), static_cast<int>(sub));
  for (int i = 0; i < 4; ++i) r[i] = static_cast<unsigned>(v[i]);
#else
  __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

bool HasCpuId() {
#if defined(_MSC_VER)
  return true;  // every CPU Windows XP runs on
#else
  return __get_cpuid_max(0, nullptr) != 0;
#endif
}

Cpu DetectCpu() {
  Cpu c;
  if (!HasCpuId()) return c;
  unsigned r[4];
  CpuId(0, 0, r);
  const unsigned max = r[0];
  std::memcpy(c.vendor, &r[1], 4);
  std::memcpy(c.vendor + 4, &r[3], 4);
  std::memcpy(c.vendor + 8, &r[2], 4);
  if (max >= 1) {
    CpuId(1, 0, r);
    c.mmx = (r[3] >> 23) & 1;
    c.sse = (r[3] >> 25) & 1;
    c.sse2 = (r[3] >> 26) & 1;
    c.sse3 = r[2] & 1;
    c.ssse3 = (r[2] >> 9) & 1;
    c.sse41 = (r[2] >> 19) & 1;
    c.sse42 = (r[2] >> 20) & 1;
    c.avx = (r[2] >> 28) & 1;
  }
  if (max >= 7) {
    CpuId(7, 0, r);
    c.avx2 = (r[1] >> 5) & 1;
  }
  CpuId(0x80000000u, 0, r);
  const unsigned ext = r[0];
  if (ext >= 0x80000001u) {
    CpuId(0x80000001u, 0, r);
    c.now3d = (r[3] >> 31) & 1;
  }
  if (ext >= 0x80000004u) {
    for (unsigned i = 0; i < 3; ++i) {
      CpuId(0x80000002u + i, 0, r);
      std::memcpy(c.brand + i * 16, r, 16);
    }
    // Trim the padding some CPUs put in front.
    char* b = c.brand;
    while (*b == ' ') ++b;
    std::memmove(c.brand, b, std::strlen(b) + 1);
  }
  return c;
}

// ---- Statistics (SimdLog) ----------------------------------------------------------
enum Kernel {
  kExpand16, kPack565, kAlpha32, kSoftAlpha, kAlphaKnown, kFirstRing, kConvert, kSamePos, kGather, kSums, kBlend,
  kKernelCount
};
const char* const kNames[kKernelCount] = {"16-bit to BGRA", "BGRA to 565", "32-bit alpha", "soft-alpha count",
                                           "alpha map", "first ring", "vertex convert", "same positions",
                                           "gather positions", "move sums", "blend positions"};
struct Stat {
  unsigned calls;
  unsigned long long items;
  long long ticks;
};
Stat g_stats[kKernelCount];
LARGE_INTEGER g_freq;

class Timer {
 public:
  Timer(Kernel k, unsigned items) : k_(k) {
    if (!g_log) return;
    g_stats[k].calls++;
    g_stats[k].items += items;
    QueryPerformanceCounter(&t0_);
  }
  ~Timer() {
    if (!g_log) return;
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_stats[k_].ticks += t1.QuadPart - t0_.QuadPart;
  }

 private:
  Kernel k_;
  LARGE_INTEGER t0_{};
};

double Ms(long long ticks) { return ticks * 1000.0 / static_cast<double>(g_freq.QuadPart); }

// ---- Self test ---------------------------------------------------------------------
uint32_t g_rand = 12345;
uint32_t Rand() {
  g_rand = g_rand * 1664525u + 1013904223u;
  return g_rand >> 8;
}

// Calls fn reps times; returns ms per call (best of 3 rounds).
template <typename F>
double TimeIt(int reps, F fn) {
  double best = 1e30;
  for (int round = 0; round < 3; ++round) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < reps; ++i) fn();
    QueryPerformanceCounter(&b);
    const double ms = Ms(b.QuadPart - a.QuadPart) / reps;
    if (ms < best) best = ms;
  }
  return best;
}

void Report(const char* what, bool same, double plain_ms, double sse_ms, const char* unit) {
  patch::Log("simd: %-16s %s  plain %.3f ms, SSE2 %.3f ms per %s (%.1fx)", what, same ? "same output" : "DIFFERENT",
             plain_ms, sse_ms, unit, sse_ms > 0 ? plain_ms / sse_ms : 0.0);
}

bool Close(double a, double b) { return std::fabs(a - b) <= 1e-4 * (1.0 + std::fabs(a) + std::fabs(b)); }

// Runs each SSE2 kernel against the plain one on made-up data: the same
// output is required (pixels exactly; vertex maths within float rounding).
// Times both when SimdLog is on.
bool SelfTest(bool time) {
  bool ok = true;
  const int reps = time ? 5 : 1;
  // 16-bit textures: the formats the game uses.
  const uint32_t formats[][4] = {{0xF800, 0x07E0, 0x001F, 0}, {0x7C00, 0x03E0, 0x001F, 0x8000},
                                 {0x0F00, 0x00F0, 0x000F, 0xF000}, {0x7C00, 0x03E0, 0x001F, 0}};
  const unsigned npx = 256 * 256 + 5;
  std::vector<uint16_t> px16(npx);
  for (auto& p : px16) p = static_cast<uint16_t>(Rand());
  for (int i = 0; i < 65536; ++i) px16[i] = static_cast<uint16_t>(i);  // every value
  std::vector<uint8_t> a(npx * 4), b(npx * 4);
  for (const auto& m : formats) {
    Format16 f;
    if (!Prepare16(f, m)) {
      patch::Log("simd: 16-bit format %04x/%04x/%04x/%04x not handled", m[0], m[1], m[2], m[3]);
      ok = false;
      continue;
    }
    plain::Expand16(f, px16.data(), a.data(), npx);
    sse2::Expand16(f, px16.data(), b.data(), npx);
    const bool same = a == b;
    ok &= same;
    if (time || !same) {
      // Without SSE2 the renderer converts through a 65536-entry table
      // (surface.cpp): time that, not the plain formula.
      std::vector<uint32_t> lut(65536);
      for (unsigned v = 0; v < 65536; ++v) {
        const uint16_t p16 = static_cast<uint16_t>(v);
        plain::Expand16(f, &p16, reinterpret_cast<uint8_t*>(&lut[v]), 1);
      }
      char what[40];
      std::snprintf(what, sizeof(what), "16-bit %04x/%04x", m[0], m[3]);
      Report(what, same, TimeIt(reps, [&] {
               for (unsigned v = 0; v < 65536; ++v) std::memcpy(&a[v * 4], &lut[px16[v]], 4);
             }),
             TimeIt(reps, [&] { sse2::Expand16(f, px16.data(), b.data(), 65536); }), "256x256 (plain = table)");
    }
  }
  // Readback.
  std::vector<uint8_t> bgra(npx * 4);
  for (auto& v : bgra) v = static_cast<uint8_t>(Rand());
  for (int i = 0; i < 256 * 3; ++i) bgra[i] = static_cast<uint8_t>(i / 3);  // every channel value
  {
    std::vector<uint16_t> p(npx), q(npx);
    plain::PackTo565(bgra.data(), p.data(), npx);
    sse2::PackTo565(bgra.data(), q.data(), npx);
    const bool same = p == q;
    ok &= same;
    if (time || !same)
      Report("BGRA to 565", same, TimeIt(reps, [&] { plain::PackTo565(bgra.data(), p.data(), 65536); }),
             TimeIt(reps, [&] { sse2::PackTo565(bgra.data(), q.data(), 65536); }), "256x256");
  }
  {
    std::vector<uint32_t> p(npx), q(npx), r(npx), s(npx);
    const uint32_t* src = reinterpret_cast<const uint32_t*>(bgra.data());
    plain::ClearAlpha32(src, p.data(), npx);
    sse2::ClearAlpha32(src, q.data(), npx);
    plain::SetAlpha32(src, r.data(), npx);
    sse2::SetAlpha32(src, s.data(), npx);
    const bool same = p == q && r == s;
    ok &= same;
    if (time || !same)
      Report("32-bit alpha", same, TimeIt(reps, [&] { plain::ClearAlpha32(src, p.data(), 65536); }),
             TimeIt(reps, [&] { sse2::ClearAlpha32(src, q.data(), 65536); }), "256x256");
  }
  // Alpha maps: a texture with clear areas.
  {
    const unsigned w = 253, h = 129, n = w * h;
    std::vector<uint8_t> tex(n * 4);
    for (unsigned i = 0; i < n; ++i) {
      tex[i * 4] = static_cast<uint8_t>(Rand());
      const unsigned x = i % w, y = i / w;
      const unsigned r = Rand() & 255;
      tex[i * 4 + 3] = static_cast<uint8_t>(((x / 17 + y / 11) & 1) ? 0 : (r < 40 ? r : 255 - (r & 7)));
    }
    const bool soft_same = plain::CountSoftAlpha(tex.data(), n) == sse2::CountSoftAlpha(tex.data(), n);
    std::vector<uint8_t> k1(n), k2(n), o1(n), o2(n), hd(n);
    const bool known_same = plain::AlphaKnown(tex.data(), k1.data(), n) == sse2::AlphaKnown(tex.data(), k2.data(), n) &&
                            k1 == k2;
    plain::FirstRing(k1.data(), o1.data(), w, h, hd.data());
    sse2::FirstRing(k1.data(), o2.data(), w, h, hd.data());
    const bool ring_same = o1 == o2;
    ok &= soft_same && known_same && ring_same;
    if (time || !soft_same)
      Report("soft-alpha count", soft_same, TimeIt(reps, [&] { plain::CountSoftAlpha(tex.data(), n); }),
             TimeIt(reps, [&] { sse2::CountSoftAlpha(tex.data(), n); }), "253x129");
    if (time || !known_same)
      Report("alpha map", known_same, TimeIt(reps, [&] { plain::AlphaKnown(tex.data(), k1.data(), n); }),
             TimeIt(reps, [&] { sse2::AlphaKnown(tex.data(), k2.data(), n); }), "253x129");
    if (time || !ring_same)
      Report("first ring", ring_same, TimeIt(reps, [&] { plain::FirstRing(k1.data(), o1.data(), w, h, hd.data()); }),
             TimeIt(reps, [&] { sse2::FirstRing(k1.data(), o2.data(), w, h, hd.data()); }), "253x129");
  }
  // Vertices: the game's common layouts (xyz diffuse uv; xyzrhw diffuse
  // specular uv; xyz normal uv uv).
  {
    const unsigned nv = 2001;
    std::vector<float> fv(nv * 12 + 4);
    for (auto& v : fv) v = static_cast<float>(static_cast<int>(Rand() & 0xFFFF) - 32768) * 0.01f;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(fv.data());
    VertexIn layouts[4] = {{24, false, 12, -1, 16, -1, true, false},
                           {32, true, 16, 20, 24, -1, true, false},
                           {48, false, -1, -1, 24, 32, true, true},
                           {20, false, -1, -1, 12, 16, false, false}};
    bool same = true;
    std::vector<uint8_t> p(nv * 40), q(nv * 40);
    for (const VertexIn& l : layouts) {
      plain::ConvertVertices(l, src, nv, p.data());
      sse2::ConvertVertices(l, src, nv, q.data());
      same &= p == q;
    }
    ok &= same;
    if (time || !same)
      Report("vertex convert", same, TimeIt(reps * 10, [&] { plain::ConvertVertices(layouts[0], src, nv, p.data()); }),
             TimeIt(reps * 10, [&] { sse2::ConvertVertices(layouts[0], src, nv, q.data()); }), "2001 verts");
    // Smoothing maths on packed positions.
    std::vector<float> prev(nv * 3), cur(nv * 3);
    for (unsigned i = 0; i < nv * 3; ++i) {
      prev[i] = fv[i];
      cur[i] = fv[i] + static_cast<float>(static_cast<int>(Rand() & 255) - 128) * 0.01f;
    }
    const float* pos = prev.data();
    std::vector<uint8_t> verts(nv * 24 + 16);
    for (unsigned i = 0; i < nv; ++i) std::memcpy(verts.data() + i * 24, pos + i * 3, 12);
    bool same_pos = plain::SamePositions(verts.data(), 24, nv, pos) && sse2::SamePositions(verts.data(), 24, nv, pos);
    verts[(nv - 1) * 24 + 11] ^= 1;  // last vertex differs
    same_pos &= !plain::SamePositions(verts.data(), 24, nv, pos) && !sse2::SamePositions(verts.data(), 24, nv, pos);
    verts[(nv - 1) * 24 + 11] ^= 1;
    verts[7 * 24 + 4] ^= 0x80;
    same_pos &= !plain::SamePositions(verts.data(), 24, nv, pos) && !sse2::SamePositions(verts.data(), 24, nv, pos);
    verts[7 * 24 + 4] ^= 0x80;
    double s1[3], s2[3];
    plain::SumPositions(pos, nv, s1);
    sse2::SumPositions(pos, nv, s2);
    bool same_sums = Close(s1[0], s2[0]) && Close(s1[1], s2[1]) && Close(s1[2], s2[2]);
    const bool m1 = plain::SumMoves(prev.data(), cur.data(), nv, 30.0f * 30.0f, s1);
    const bool m2 = sse2::SumMoves(prev.data(), cur.data(), nv, 30.0f * 30.0f, s2);
    same_sums &= m1 && m2 && Close(s1[0], s2[0]) && Close(s1[1], s2[1]) && Close(s1[2], s2[2]);
    same_sums &= !plain::SumMoves(prev.data(), cur.data(), nv, 0.5f, s1) && !sse2::SumMoves(prev.data(), cur.data(), nv, 0.5f, s2);
    for (double& v : s1) v /= nv;
    same_sums &= Close(plain::MaxBend(prev.data(), cur.data(), nv, s1), sse2::MaxBend(prev.data(), cur.data(), nv, s1));
    std::vector<uint8_t> v1 = verts, v2 = verts;
    plain::BlendPositions(prev.data(), cur.data(), nv, 0.37f, v1.data(), 24);
    sse2::BlendPositions(prev.data(), cur.data(), nv, 0.37f, v2.data(), 24);
    bool same_blend = true;
    for (unsigned i = 0; i < nv * 24 && same_blend; i += 4) {
      float x, y;
      std::memcpy(&x, v1.data() + i, 4);
      std::memcpy(&y, v2.data() + i, 4);
      same_blend = std::fabs(x - y) <= 1e-3f * (1.0f + std::fabs(x));
    }
    ok &= same_pos && same_sums && same_blend;
    if (time || !same_pos)
      Report("same positions", same_pos, TimeIt(reps * 10, [&] { plain::SamePositions(verts.data(), 24, nv, pos); }),
             TimeIt(reps * 10, [&] { sse2::SamePositions(verts.data(), 24, nv, pos); }), "2001 verts");
    if (time || !same_sums)
      Report("move sums", same_sums, TimeIt(reps * 10, [&] {
               plain::SumMoves(prev.data(), cur.data(), nv, 900.0f, s1);
               plain::MaxBend(prev.data(), cur.data(), nv, s2);
             }),
             TimeIt(reps * 10, [&] {
               sse2::SumMoves(prev.data(), cur.data(), nv, 900.0f, s1);
               sse2::MaxBend(prev.data(), cur.data(), nv, s2);
             }), "2001 verts");
    if (time || !same_blend)
      Report("blend positions", same_blend, TimeIt(reps * 10, [&] { plain::BlendPositions(prev.data(), cur.data(), nv, 0.37f, v1.data(), 24); }),
             TimeIt(reps * 10, [&] { sse2::BlendPositions(prev.data(), cur.data(), nv, 0.37f, v2.data(), 24); }),
             "2001 verts");
  }
  return ok;
}

}  // namespace

void Init() {
  if (g_init) return;
  g_init = true;
  QueryPerformanceFrequency(&g_freq);
  const Config& cfg = GetConfig();
  g_log = cfg.simd_log;
  const Cpu c = DetectCpu();
  // The OS must also save SSE registers on task switches (every Windows
  // since 98 does; this asks it directly).
  const bool os_sse2 = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) != 0;
  patch::Log("simd: CPU \"%s\" (%s): MMX %d 3DNow %d SSE %d SSE2 %d SSE3 %d SSSE3 %d SSE4.1 %d SSE4.2 %d AVX %d AVX2 %d; "
             "Windows reports SSE2 %d",
             c.brand[0] ? c.brand : "?", c.vendor, c.mmx, c.now3d, c.sse, c.sse2, c.sse3, c.ssse3, c.sse41, c.sse42,
             c.avx, c.avx2, os_sse2);
  const bool can = c.sse2 && os_sse2;
  if (!cfg.simd) {
    patch::Log("simd: Simd=0, plain code paths (no SSE2)");
  } else if (!can) {
    patch::Log("simd: no SSE2 on this CPU, plain code paths");
  } else {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool ok = SelfTest(g_log);
    QueryPerformanceCounter(&t1);
    g_sse2 = ok;
    patch::Log("simd: self test %s in %.1f ms; %s", ok ? "passed" : "FAILED", Ms(t1.QuadPart - t0.QuadPart),
               ok ? "SSE2 code paths in use" : "plain code paths in use");
  }
}

bool Sse2() { return g_sse2; }
const char* PathName() { return g_sse2 ? "SSE2" : "plain"; }

void ReportStats() {
  if (!g_log) return;
  static DWORD last = GetTickCount();
  const DWORD now = GetTickCount();
  if (now - last < 5000) return;
  const double secs = (now - last) / 1000.0;
  last = now;
  char line[1024];
  int len = std::snprintf(line, sizeof(line), "simd %s %.1fs:", PathName(), secs);
  bool any = false;
  for (int k = 0; k < kKernelCount; ++k) {
    const Stat& s = g_stats[k];
    if (!s.calls || len >= static_cast<int>(sizeof(line)) - 1) continue;
    len += std::snprintf(line + len, sizeof(line) - len, "%s %s %u calls %llu items %.2f ms", any ? ";" : "",
                         kNames[k], s.calls, s.items, Ms(s.ticks));
    any = true;
  }
  std::memset(g_stats, 0, sizeof(g_stats));
  if (any) patch::Log("%s", line);
}

// ---- Dispatch ----------------------------------------------------------------------
#define DISPATCH(fn, ...) (g_sse2 ? sse2::fn(__VA_ARGS__) : plain::fn(__VA_ARGS__))

void Expand16(const Format16& f, const uint16_t* src, uint8_t* dst, unsigned n) {
  Timer t(kExpand16, n);
  DISPATCH(Expand16, f, src, dst, n);
}
void PackTo565(const uint8_t* src, uint16_t* dst, unsigned n) {
  Timer t(kPack565, n);
  DISPATCH(PackTo565, src, dst, n);
}
void ClearAlpha32(const uint32_t* src, uint32_t* dst, unsigned n) {
  Timer t(kAlpha32, n);
  DISPATCH(ClearAlpha32, src, dst, n);
}
void SetAlpha32(const uint32_t* src, uint32_t* dst, unsigned n) {
  Timer t(kAlpha32, n);
  DISPATCH(SetAlpha32, src, dst, n);
}
unsigned CountSoftAlpha(const uint8_t* bgra, unsigned n) {
  Timer t(kSoftAlpha, n);
  return DISPATCH(CountSoftAlpha, bgra, n);
}
unsigned AlphaKnown(const uint8_t* bgra, uint8_t* known, unsigned n) {
  Timer t(kAlphaKnown, n);
  return DISPATCH(AlphaKnown, bgra, known, n);
}
void FirstRing(const uint8_t* known, uint8_t* out, unsigned w, unsigned h) {
  Timer t(kFirstRing, w * h);
  std::vector<uint8_t> hd(static_cast<size_t>(w) * h);
  DISPATCH(FirstRing, known, out, w, h, hd.data());
}
void ConvertVertices(const VertexIn& in, const uint8_t* src, uint32_t count, void* out) {
  Timer t(kConvert, count);
  DISPATCH(ConvertVertices, in, src, count, out);
}
void GatherPositions(const uint8_t* verts, uint32_t stride, uint32_t count, float* pos) {
  Timer t(kGather, count);
  for (uint32_t i = 0; i < count; ++i, verts += stride) std::memcpy(pos + i * 3, verts, 12);
}
bool SamePositions(const uint8_t* verts, uint32_t stride, uint32_t count, const float* pos) {
  Timer t(kSamePos, count);
  return DISPATCH(SamePositions, verts, stride, count, pos);
}
void SumPositions(const float* pos, uint32_t count, double sum[3]) {
  Timer t(kSums, count);
  DISPATCH(SumPositions, pos, count, sum);
}
bool SumMoves(const float* prev, const float* cur, uint32_t count, float max2, double sum[3]) {
  Timer t(kSums, count);
  return DISPATCH(SumMoves, prev, cur, count, max2, sum);
}
double MaxBend(const float* prev, const float* cur, uint32_t count, const double mean[3]) {
  Timer t(kSums, count);
  return DISPATCH(MaxBend, prev, cur, count, mean);
}
void BlendPositions(const float* prev, const float* cur, uint32_t count, float alpha, uint8_t* verts,
                    uint32_t stride) {
  Timer t(kBlend, count);
  DISPATCH(BlendPositions, prev, cur, count, alpha, verts, stride);
}

}  // namespace simd
