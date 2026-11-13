// SSE2 kernels (see simd.h). Compiled with -msse2 -mstackrealign on GCC, and
// only called once simd.cpp has found SSE2 on the CPU. No C++ library
// headers here (see simd_impl.h).

#include <emmintrin.h>
#include <string.h>

#include "simd_impl.h"

namespace simd {
namespace sse2 {

namespace {

inline __m128i Load(const void* p) { return _mm_loadu_si128(static_cast<const __m128i*>(p)); }
inline void Store(void* p, __m128i v) { _mm_storeu_si128(static_cast<__m128i*>(p), v); }

// x y z of a point, w = 0 (12 bytes read).
inline __m128 Load3(const float* p) {
  const __m128 xy = _mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)));
  return _mm_movelh_ps(xy, _mm_load_ss(p + 2));
}
// x y z and one float more (16 bytes read; the caller knows they exist).
inline __m128 Load4(const float* p) { return _mm_loadu_ps(p); }
inline void Store3(uint8_t* p, __m128 v) {
  _mm_storel_epi64(reinterpret_cast<__m128i*>(p), _mm_castps_si128(v));
  _mm_store_ss(reinterpret_cast<float*>(p + 8), _mm_movehl_ps(v, v));
}

}  // namespace

// ---- 16-bit pixels -----------------------------------------------------------
namespace {
// One channel of 8 pixels widened to 8 bits (see Expand16).
struct Channel {
  __m128i mask, half, mul, shift, post;
  bool present, divide;
  __m128i fill;  // value when absent (0, or 255 for alpha)
};
inline Channel MakeChannel(const Format16& f, int c) {
  Channel ch;
  const unsigned max = f.bits[c] ? (1u << f.bits[c]) - 1 : 0;
  ch.mask = _mm_set1_epi16(static_cast<short>(f.mask[c]));
  ch.half = _mm_set1_epi16(static_cast<short>(max / 2));
  ch.mul = _mm_set1_epi16(static_cast<short>(f.mul[c]));
  ch.shift = _mm_cvtsi32_si128(f.shift[c]);
  ch.post = _mm_cvtsi32_si128(f.post[c]);
  ch.present = f.bits[c] != 0;
  ch.divide = f.mul[c] != 0;
  ch.fill = c == 3 ? _mm_set1_epi16(255) : _mm_setzero_si128();
  return ch;
}
inline __m128i Widen(const Channel& ch, __m128i px) {
  if (!ch.present) return ch.fill;
  const __m128i raw = _mm_srl_epi16(_mm_and_si128(px, ch.mask), ch.shift);
  const __m128i x = _mm_add_epi16(_mm_sub_epi16(_mm_slli_epi16(raw, 8), raw), ch.half);  // raw * 255 + max / 2
  return ch.divide ? _mm_srl_epi16(_mm_mulhi_epu16(x, ch.mul), ch.post) : x;  // one bit: raw * 255
}
}  // namespace

void Expand16(const Format16& f, const uint16_t* src, uint8_t* dst, unsigned n) {
  // Per channel: raw = (px & mask) >> shift; v = ((raw * 255 + max / 2) * mul) >> (16 + post).
  unsigned i = 0;
  if (f.mask[0] == 0xF800 && f.mask[1] == 0x07E0 && f.mask[2] == 0x001F && !f.mask[3] && f.mul[0] && f.mul[1]) {
    // R5G6B5 (most of the game's textures): fixed shifts.
    const __m128i m5 = _mm_set1_epi16(0x1F), m6 = _mm_set1_epi16(0x3F);
    const __m128i h5 = _mm_set1_epi16(15), h6 = _mm_set1_epi16(31);
    const __m128i k5 = _mm_set1_epi16(static_cast<short>(f.mul[0])), k6 = _mm_set1_epi16(static_cast<short>(f.mul[1]));
    const __m128i p5 = _mm_cvtsi32_si128(f.post[0]), p6 = _mm_cvtsi32_si128(f.post[1]);
    const __m128i alpha = _mm_set1_epi16(static_cast<short>(0xFF00));
    auto widen = [](__m128i raw, __m128i half, __m128i k, __m128i post) {
      return _mm_srl_epi16(_mm_mulhi_epu16(_mm_add_epi16(_mm_sub_epi16(_mm_slli_epi16(raw, 8), raw), half), k), post);
    };
    for (; i + 8 <= n; i += 8) {
      const __m128i px = Load(src + i);
      const __m128i b5 = widen(_mm_and_si128(px, m5), h5, k5, p5);
      const __m128i g6 = widen(_mm_and_si128(_mm_srli_epi16(px, 5), m6), h6, k6, p6);
      const __m128i r5 = widen(_mm_srli_epi16(px, 11), h5, k5, p5);
      const __m128i bg = _mm_or_si128(b5, _mm_slli_epi16(g6, 8));
      const __m128i ra = _mm_or_si128(r5, alpha);
      Store(dst + i * 4, _mm_unpacklo_epi16(bg, ra));
      Store(dst + i * 4 + 16, _mm_unpackhi_epi16(bg, ra));
    }
    if (i < n) plain::Expand16(f, src + i, dst + i * 4, n - i);
    return;
  }
  const Channel r = MakeChannel(f, 0), g = MakeChannel(f, 1), b = MakeChannel(f, 2), a = MakeChannel(f, 3);
  for (; i + 8 <= n; i += 8) {
    const __m128i px = Load(src + i);
    const __m128i bg = _mm_or_si128(Widen(b, px), _mm_slli_epi16(Widen(g, px), 8));
    const __m128i ra = _mm_or_si128(Widen(r, px), _mm_slli_epi16(Widen(a, px), 8));
    Store(dst + i * 4, _mm_unpacklo_epi16(bg, ra));
    Store(dst + i * 4 + 16, _mm_unpackhi_epi16(bg, ra));
  }
  if (i < n) plain::Expand16(f, src + i, dst + i * 4, n - i);
}

// ---- Readback ------------------------------------------------------------------
void PackTo565(const uint8_t* src, uint16_t* dst, unsigned n) {
  const __m128i lo8 = _mm_set1_epi32(0xFF);
  const __m128i k31 = _mm_set1_epi16(31), k63 = _mm_set1_epi16(63), k127 = _mm_set1_epi16(127);
  const __m128i one = _mm_set1_epi16(1);
  // (v * max + 127) / 255 for v * max + 127 < 16200: (x + 1 + (x >> 8)) >> 8.
  auto div255 = [&](__m128i x) { return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(x, one), _mm_srli_epi16(x, 8)), 8); };
  unsigned i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m128i a = Load(src + i * 4), b = Load(src + i * 4 + 16);
    const __m128i bl = _mm_packs_epi32(_mm_and_si128(a, lo8), _mm_and_si128(b, lo8));
    const __m128i gr = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 8), lo8), _mm_and_si128(_mm_srli_epi32(b, 8), lo8));
    const __m128i rd = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 16), lo8), _mm_and_si128(_mm_srli_epi32(b, 16), lo8));
    const __m128i b5 = div255(_mm_add_epi16(_mm_mullo_epi16(bl, k31), k127));
    const __m128i g6 = div255(_mm_add_epi16(_mm_mullo_epi16(gr, k63), k127));
    const __m128i r5 = div255(_mm_add_epi16(_mm_mullo_epi16(rd, k31), k127));
    Store(dst + i, _mm_or_si128(_mm_or_si128(_mm_slli_epi16(r5, 11), _mm_slli_epi16(g6, 5)), b5));
  }
  if (i < n) plain::PackTo565(src + i * 4, dst + i, n - i);
}

void ClearAlpha32(const uint32_t* src, uint32_t* dst, unsigned n) {
  const __m128i m = _mm_set1_epi32(0x00FFFFFF);
  unsigned i = 0;
  for (; i + 4 <= n; i += 4) Store(dst + i, _mm_and_si128(Load(src + i), m));
  for (; i < n; ++i) dst[i] = src[i] & 0x00FFFFFFu;
}

void SetAlpha32(const uint32_t* src, uint32_t* dst, unsigned n) {
  const __m128i m = _mm_set1_epi32(static_cast<int>(0xFF000000u));
  unsigned i = 0;
  for (; i + 4 <= n; i += 4) Store(dst + i, _mm_or_si128(Load(src + i), m));
  for (; i < n; ++i) dst[i] = src[i] | 0xFF000000u;
}

// ---- Alpha -------------------------------------------------------------------
namespace {
inline unsigned Bits4(int m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }
inline unsigned Bits16(int m) {
  unsigned v = static_cast<unsigned>(m);
  v = v - ((v >> 1) & 0x5555u);
  v = (v & 0x3333u) + ((v >> 2) & 0x3333u);
  v = (v + (v >> 4)) & 0x0F0Fu;
  return (v + (v >> 8)) & 0x1Fu;
}
}  // namespace

unsigned CountSoftAlpha(const uint8_t* bgra, unsigned n) {
  const __m128i lo = _mm_set1_epi32(8), hi = _mm_set1_epi32(247);
  unsigned count = 0, i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m128i a = _mm_srli_epi32(Load(bgra + i * 4), 24);
    const __m128i in = _mm_and_si128(_mm_cmpgt_epi32(a, lo), _mm_cmplt_epi32(a, hi));
    count += Bits4(_mm_movemask_ps(_mm_castsi128_ps(in)));
  }
  return count + plain::CountSoftAlpha(bgra + i * 4, n - i);
}

unsigned AlphaKnown(const uint8_t* bgra, uint8_t* known, unsigned n) {
  const __m128i zero = _mm_setzero_si128(), one = _mm_set1_epi8(1);
  unsigned unknown = 0, i = 0;
  for (; i + 16 <= n; i += 16) {
    __m128i z[4];
    for (int k = 0; k < 4; ++k) z[k] = _mm_cmpeq_epi32(_mm_srli_epi32(Load(bgra + (i + k * 4) * 4), 24), zero);
    const __m128i z16 = _mm_packs_epi16(_mm_packs_epi32(z[0], z[1]), _mm_packs_epi32(z[2], z[3]));  // -1 where clear
    Store(known + i, _mm_add_epi8(z16, one));
    unknown += Bits16(_mm_movemask_epi8(z16));
  }
  return unknown + plain::AlphaKnown(bgra + i * 4, known + i, n - i);
}

void FirstRing(const uint8_t* known, uint8_t* out, unsigned w, unsigned h, uint8_t* hd) {
  // hd: each row's known spread one texel left and right.
  for (unsigned y = 0; y < h; ++y) {
    const uint8_t* k = known + static_cast<size_t>(y) * w;
    uint8_t* d = hd + static_cast<size_t>(y) * w;
    unsigned x = 0;
    if (w >= 18) {
      d[0] = static_cast<uint8_t>(k[0] | k[1]);
      for (x = 1; x + 17 <= w; x += 16)
        Store(d + x, _mm_or_si128(_mm_or_si128(Load(k + x - 1), Load(k + x)), Load(k + x + 1)));
    }
    for (; x < w; ++x)
      d[x] = static_cast<uint8_t>((x ? k[x - 1] : 0) | k[x] | (x + 1 < w ? k[x + 1] : 0));
  }
  const __m128i one = _mm_set1_epi8(1);
  for (unsigned y = 0; y < h; ++y) {
    const uint8_t* up = y ? hd + static_cast<size_t>(y - 1) * w : nullptr;
    const uint8_t* mid = hd + static_cast<size_t>(y) * w;
    const uint8_t* dn = y + 1 < h ? hd + static_cast<size_t>(y + 1) * w : nullptr;
    const uint8_t* k = known + static_cast<size_t>(y) * w;
    uint8_t* o = out + static_cast<size_t>(y) * w;
    unsigned x = 0;
    for (; x + 16 <= w; x += 16) {
      __m128i v = Load(mid + x);
      if (up) v = _mm_or_si128(v, Load(up + x));
      if (dn) v = _mm_or_si128(v, Load(dn + x));
      Store(o + x, _mm_andnot_si128(Load(k + x), _mm_and_si128(v, one)));
    }
    for (; x < w; ++x) {
      const uint8_t v = static_cast<uint8_t>(mid[x] | (up ? up[x] : 0) | (dn ? dn[x] : 0));
      o[x] = static_cast<uint8_t>(v & ~k[x] & 1);
    }
  }
}

// ---- Vertices ------------------------------------------------------------------
void ConvertVertices(const VertexIn& in, const uint8_t* src, uint32_t count, void* out) {
  // One-float texture coordinates would need an 8-byte read past them.
  if ((in.t0 >= 0 && !in.t0_uv) || (in.t1 >= 0 && !in.t1_uv)) return plain::ConvertVertices(in, src, count, out);
  // Absent fields read a default instead, without stepping.
  const uint8_t* dp = in.diffuse >= 0 ? src + in.diffuse : reinterpret_cast<const uint8_t*>(&kWhite);
  const uint8_t* sp = in.specular >= 0 ? src + in.specular : reinterpret_cast<const uint8_t*>(kZeros);
  const uint8_t* t0 = in.t0 >= 0 ? src + in.t0 : reinterpret_cast<const uint8_t*>(kZeros);
  const uint8_t* t1 = in.t1 >= 0 ? src + in.t1 : reinterpret_cast<const uint8_t*>(kZeros);
  const uint32_t s = in.stride;
  const uint32_t ds = in.diffuse >= 0 ? s : 0, ss = in.specular >= 0 ? s : 0;
  const uint32_t t0s = in.t0 >= 0 ? s : 0, t1s = in.t1 >= 0 ? s : 0;
  uint8_t* o = static_cast<uint8_t*>(out);
  const __m128 w1 = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
  for (uint32_t i = 0; i < count; ++i, src += s, dp += ds, sp += ss, t0 += t0s, t1 += t1s, o += 40) {
    const float* f = reinterpret_cast<const float*>(src);
    _mm_storeu_ps(reinterpret_cast<float*>(o), in.xyzw ? _mm_loadu_ps(f) : _mm_or_ps(Load3(f), w1));
    const __m128i colours = _mm_unpacklo_epi32(_mm_cvtsi32_si128(*reinterpret_cast<const int*>(dp)),
                                               _mm_cvtsi32_si128(*reinterpret_cast<const int*>(sp)));
    Store(o + 16, _mm_unpacklo_epi64(colours, _mm_loadl_epi64(reinterpret_cast<const __m128i*>(t0))));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(o + 32), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(t1)));
  }
}

bool SamePositions(const uint8_t* verts, uint32_t stride, uint32_t count, const float* pos) {
  if (!count) return true;
  // 16 bytes at a time (they exist for all but the last vertex); only the
  // first 12 bytes count.
  uint32_t i = 0;
  for (; i + 1 < count; ++i) {
    const __m128i a = Load(verts + static_cast<size_t>(i) * stride), b = Load(pos + i * 3);
    if ((_mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) & 0x0FFF) != 0x0FFF) return false;
  }
  for (; i < count; ++i)
    if (memcmp(verts + static_cast<size_t>(i) * stride, pos + i * 3, 12) != 0) return false;
  return true;
}

void SumPositions(const float* pos, uint32_t count, double sum[3]) {
  __m128d xy = _mm_setzero_pd(), z = _mm_setzero_pd();
  for (uint32_t i = 0; i < count; ++i) {
    const __m128 p = i + 1 < count ? Load4(pos + i * 3) : Load3(pos + i * 3);
    xy = _mm_add_pd(xy, _mm_cvtps_pd(p));
    z = _mm_add_sd(z, _mm_cvtps_pd(_mm_movehl_ps(p, p)));
  }
  double a[2];
  _mm_storeu_pd(a, xy);
  sum[0] = a[0];
  sum[1] = a[1];
  sum[2] = _mm_cvtsd_f64(z);
}

bool SumMoves(const float* prev, const float* cur, uint32_t count, float max2, double sum[3]) {
  __m128d xy = _mm_setzero_pd(), z = _mm_setzero_pd();
  const __m128 lim = _mm_set_ss(max2);
  for (uint32_t i = 0; i < count; ++i) {
    const bool wide = i + 1 < count;
    const __m128 d = _mm_sub_ps(wide ? Load4(cur + i * 3) : Load3(cur + i * 3),
                                wide ? Load4(prev + i * 3) : Load3(prev + i * 3));
    const __m128 d2 = _mm_mul_ps(d, d);
    const __m128 len = _mm_add_ss(_mm_add_ss(d2, _mm_shuffle_ps(d2, d2, 1)), _mm_movehl_ps(d2, d2));
    if (_mm_comigt_ss(len, lim)) return false;
    xy = _mm_add_pd(xy, _mm_cvtps_pd(d));
    z = _mm_add_sd(z, _mm_cvtps_pd(_mm_movehl_ps(d, d)));
  }
  double a[2];
  _mm_storeu_pd(a, xy);
  sum[0] = a[0];
  sum[1] = a[1];
  sum[2] = _mm_cvtsd_f64(z);
  return true;
}

double MaxBend(const float* prev, const float* cur, uint32_t count, const double mean[3]) {
  const __m128d mxy = _mm_loadu_pd(mean), mz = _mm_load_sd(mean + 2);
  __m128d best = _mm_setzero_pd();
  for (uint32_t i = 0; i < count; ++i) {
    const bool wide = i + 1 < count;
    const __m128 d = _mm_sub_ps(wide ? Load4(cur + i * 3) : Load3(cur + i * 3),
                                wide ? Load4(prev + i * 3) : Load3(prev + i * 3));
    const __m128d exy = _mm_sub_pd(_mm_cvtps_pd(d), mxy);
    const __m128d ez = _mm_sub_sd(_mm_cvtps_pd(_mm_movehl_ps(d, d)), mz);
    const __m128d sq = _mm_mul_pd(exy, exy);
    const __m128d d2 = _mm_add_sd(_mm_add_sd(sq, _mm_unpackhi_pd(sq, sq)), _mm_mul_sd(ez, ez));
    best = _mm_max_sd(d2, best);  // a NaN never wins, as in the plain loop
  }
  return _mm_cvtsd_f64(best);
}

void BlendPositions(const float* prev, const float* cur, uint32_t count, float alpha, uint8_t* verts,
                    uint32_t stride) {
  const __m128 a = _mm_set1_ps(alpha);
  for (uint32_t i = 0; i < count; ++i) {
    const bool wide = i + 1 < count;
    const __m128 p = wide ? Load4(prev + i * 3) : Load3(prev + i * 3);
    const __m128 c = wide ? Load4(cur + i * 3) : Load3(cur + i * 3);
    Store3(verts + static_cast<size_t>(i) * stride, _mm_add_ps(p, _mm_mul_ps(_mm_sub_ps(c, p), a)));
  }
}

}  // namespace sse2
}  // namespace simd
