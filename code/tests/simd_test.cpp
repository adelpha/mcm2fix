// Plain vs SSE2 kernels (simd_plain.cpp, simd_sse2.cpp) on the build machine:
//   g++ -O2 -msse2 -Isrc tests/simd_test.cpp src/simd_plain.cpp src/simd_sse2.cpp
// Exhaustive for the pixel formats; random data for the rest.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "simd_impl.h"

using namespace simd;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// The renderer's reference conversion (PixelLayout::ToBGRA / FromBGRA).
static void RefToBGRA(const uint32_t m[4], unsigned px, uint8_t out[4]) {
  static const int order[4] = {2, 1, 0, 3};
  for (int c = 0; c < 4; ++c) {
    uint8_t v;
    if (!m[c]) v = c == 3 ? 255 : 0;
    else {
      unsigned s = 0, b = 0;
      while (!((m[c] >> s) & 1)) ++s;
      while ((m[c] >> (s + b)) & 1) ++b;
      unsigned raw = (px & m[c]) >> s, max = (1u << b) - 1;
      v = b >= 8 ? raw >> (b - 8) : (raw <= max ? (raw * 255 + max / 2) / max : 255);
    }
    out[order[c]] = v;
  }
}

int main() {
  std::mt19937 rng(7);
  const uint32_t formats[][4] = {{0xF800, 0x07E0, 0x001F, 0}, {0x7C00, 0x03E0, 0x001F, 0x8000},
                                 {0x0F00, 0x00F0, 0x000F, 0xF000}, {0x7C00, 0x03E0, 0x001F, 0},
                                 {0x00E0, 0x001C, 0x0003, 0xFF00}, {0x001F, 0x07E0, 0xF800, 0}};
  std::vector<uint16_t> all(65536 + 7);
  for (unsigned i = 0; i < all.size(); ++i) all[i] = static_cast<uint16_t>(i * 40503u);
  for (unsigned i = 0; i < 65536; ++i) all[i] = static_cast<uint16_t>(i);
  for (auto& m : formats) {
    Format16 f;
    CHECK(Prepare16(f, m), "prepare %x", m[0]);
    std::vector<uint8_t> a(all.size() * 4), b(all.size() * 4), r(all.size() * 4);
    for (unsigned i = 0; i < all.size(); ++i) RefToBGRA(m, all[i], &r[i * 4]);
    plain::Expand16(f, all.data(), a.data(), all.size());
    for (unsigned off = 0; off < 3; ++off) {
      sse2::Expand16(f, all.data() + off, b.data() + off * 4, all.size() - off);
      CHECK(std::memcmp(a.data() + off * 4, b.data() + off * 4, (all.size() - off) * 4) == 0, "expand sse2 %x off %u", m[0], off);
    }
    CHECK(a == r, "expand plain vs reference %x", m[0]);
  }
  // 565 pack vs FromBGRA rounding, all channel values.
  std::vector<uint8_t> px(4 * 70001);
  for (auto& v : px) v = rng();
  for (int i = 0; i < 256; ++i) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = i;
  std::vector<uint16_t> p(70001), q(70001);
  plain::PackTo565(px.data(), p.data(), 70001);
  sse2::PackTo565(px.data(), q.data(), 70001);
  CHECK(p == q, "pack565");
  for (int i = 0; i < 70001; ++i) {
    unsigned b = (px[i*4] * 31 + 127) / 255, g = (px[i*4+1] * 63 + 127) / 255, r = (px[i*4+2] * 31 + 127) / 255;
    if (p[i] != ((r << 11) | (g << 5) | b)) { CHECK(false, "pack565 ref %d", i); break; }
  }
  // Alpha.
  for (unsigned w : {1u, 5u, 16u, 17u, 18u, 33u, 64u, 255u}) for (unsigned h : {1u, 2u, 3u, 31u, 64u}) {
    unsigned n = w * h;
    std::vector<uint8_t> t(n * 4);
    for (unsigned i = 0; i < n; ++i) { t[i*4+3] = (rng() % 3 == 0) ? 0 : rng(); }
    CHECK(plain::CountSoftAlpha(t.data(), n) == sse2::CountSoftAlpha(t.data(), n), "soft %ux%u", w, h);
    std::vector<uint8_t> k1(n), k2(n), o1(n), o2(n), hd(n), ref(n);
    CHECK(plain::AlphaKnown(t.data(), k1.data(), n) == sse2::AlphaKnown(t.data(), k2.data(), n) && k1 == k2, "known %ux%u", w, h);
    plain::FirstRing(k1.data(), o1.data(), w, h, hd.data());
    sse2::FirstRing(k1.data(), o2.data(), w, h, hd.data());
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      bool nb = false;
      for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        int nx = x + dx, ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < (int)w && ny < (int)h && k1[ny * w + nx]) nb = true;
      }
      ref[y * w + x] = !k1[y * w + x] && nb;
    }
    CHECK(o1 == ref && o2 == ref, "ring %ux%u", w, h);
  }
  // Vertices.
  const unsigned nv = 777;
  std::vector<float> fv(nv * 16 + 4);
  for (auto& v : fv) v = std::uniform_real_distribution<float>(-1000, 1000)(rng);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(fv.data());
  VertexIn layouts[] = {{24, false, 12, -1, 16, -1, true, false}, {32, true, 16, 20, 24, -1, true, false},
                        {48, false, -1, -1, 24, 32, true, true}, {20, false, -1, -1, 12, 16, false, false},
                        {12, false, -1, -1, -1, -1, false, false}, {36, true, 16, 20, 24, 28, false, true}};
  for (auto& l : layouts) {
    std::vector<uint8_t> a(nv * 40), b(nv * 40);
    plain::ConvertVertices(l, src, nv, a.data());
    sse2::ConvertVertices(l, src, nv, b.data());
    CHECK(a == b, "convert stride %u", l.stride);
    // Reference: the device's old per-field loop.
    for (unsigned i = 0; i < nv; ++i) {
      const uint8_t* s = src + i * l.stride; uint32_t v[10];
      std::memcpy(v, s, 12); if (l.xyzw) std::memcpy(v + 3, s + 12, 4); else { float one = 1; std::memcpy(v + 3, &one, 4); }
      v[4] = 0xffffffff; v[5] = 0; if (l.diffuse >= 0) std::memcpy(v + 4, s + l.diffuse, 4);
      if (l.specular >= 0) std::memcpy(v + 5, s + l.specular, 4);
      v[6] = v[7] = v[8] = v[9] = 0;
      if (l.t0 >= 0) std::memcpy(v + 6, s + l.t0, l.t0_uv ? 8 : 4);
      if (l.t1 >= 0) std::memcpy(v + 8, s + l.t1, l.t1_uv ? 8 : 4);
      if (std::memcmp(v, a.data() + i * 40, 40)) { CHECK(false, "convert ref stride %u vertex %u", l.stride, i); break; }
    }
  }
  for (unsigned n : {1u, 2u, 3u, 100u}) for (unsigned stride : {12u, 16u, 28u}) {
    std::vector<float> pos(n * 3), cur(n * 3);
    for (unsigned i = 0; i < n * 3; ++i) { pos[i] = fv[i]; cur[i] = fv[i] + std::uniform_real_distribution<float>(-2, 2)(rng); }
    std::vector<uint8_t> verts(n * stride);
    for (unsigned i = 0; i < n; ++i) std::memcpy(&verts[i * stride], &pos[i * 3], 12);
    CHECK(plain::SamePositions(verts.data(), stride, n, pos.data()) && sse2::SamePositions(verts.data(), stride, n, pos.data()), "same %u", n);
    for (unsigned byte = 0; byte < 12; ++byte) {
      unsigned at = (n - 1) * stride + byte;
      verts[at] ^= 4;
      CHECK(!plain::SamePositions(verts.data(), stride, n, pos.data()) && !sse2::SamePositions(verts.data(), stride, n, pos.data()), "differ %u byte %u", n, byte);
      verts[at] ^= 4;
    }
    double a[3], b[3];
    plain::SumPositions(pos.data(), n, a); sse2::SumPositions(pos.data(), n, b);
    for (int c = 0; c < 3; ++c) CHECK(std::fabs(a[c] - b[c]) < 1e-6 * (1 + std::fabs(a[c])), "sum");
    CHECK(plain::SumMoves(pos.data(), cur.data(), n, 900, a) && sse2::SumMoves(pos.data(), cur.data(), n, 900, b), "moves");
    for (int c = 0; c < 3; ++c) CHECK(std::fabs(a[c] - b[c]) < 1e-4, "move sum");
    CHECK(!plain::SumMoves(pos.data(), cur.data(), n, 1e-9f, a) && !sse2::SumMoves(pos.data(), cur.data(), n, 1e-9f, b), "moves limit");
    double mean[3] = {0.1, -0.2, 0.3};
    CHECK(std::fabs(plain::MaxBend(pos.data(), cur.data(), n, mean) - sse2::MaxBend(pos.data(), cur.data(), n, mean)) < 1e-4, "bend");
    std::vector<uint8_t> v1 = verts, v2 = verts;
    plain::BlendPositions(pos.data(), cur.data(), n, 0.3f, v1.data(), stride);
    sse2::BlendPositions(pos.data(), cur.data(), n, 0.3f, v2.data(), stride);
    for (unsigned i = 0; i < n * stride; i += 4) {
      float x, y; std::memcpy(&x, &v1[i], 4); std::memcpy(&y, &v2[i], 4);
      if (std::fabs(x - y) > 1e-3f) { CHECK(false, "blend %u", i); break; }
    }
    if (stride > 12) CHECK(std::memcmp(&v1[12], &verts[12], stride - 12) == 0 && std::memcmp(&v2[12], &verts[12], stride - 12) == 0, "blend wrote past position");
  }
  std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
  return g_fail != 0;
}
