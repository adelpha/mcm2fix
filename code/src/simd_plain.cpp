// Plain (no SSE2) kernels behind simd.h, also the reference the SSE2 ones
// are tested against. Built for any i686 CPU.

#include <cstring>

#include "simd_impl.h"

namespace simd {

const uint32_t kWhite = 0xFFFFFFFFu;
const uint32_t kZeros[4] = {0, 0, 0, 0};

bool Prepare16(Format16& f, const uint32_t mask[4]) {
  std::memset(&f, 0, sizeof(f));
  f.ok = true;
  for (int c = 0; c < 4; ++c) {
    f.mask[c] = mask[c];
    if (mask[c] > 0xFFFF) f.ok = false;
    if (!mask[c]) continue;
    unsigned s = 0;
    while (!((mask[c] >> s) & 1)) ++s;
    unsigned b = 0;
    while ((mask[c] >> (s + b)) & 1) ++b;
    if ((mask[c] >> s) != (1u << b) - 1) f.ok = false;  // not one run of bits
    f.shift[c] = static_cast<uint8_t>(s);
    f.bits[c] = static_cast<uint8_t>(b);
    if (b > 8) {
      f.ok = false;
      continue;
    }
    // The rounding divide (v * 255 + max / 2) / max as (x * mul) >> (16 + post),
    // checked for every v. One bit needs no divide (mul 0: v * 255).
    const unsigned max = (1u << b) - 1;
    bool found = b == 1;
    for (unsigned post = 0; post < 8 && !found; ++post) {
      const unsigned long long k = ((1ull << (16 + post)) + max - 1) / max;  // ceil
      for (unsigned long long mul = k; mul <= k + 1 && mul < 65536 && !found; ++mul) {
        bool all = true;
        for (unsigned v = 0; v <= max && all; ++v) {
          const unsigned x = v * 255 + max / 2;
          all = ((x * mul) >> (16 + post)) == x / max;
        }
        if (all) {
          f.mul[c] = static_cast<uint16_t>(mul);
          f.post[c] = static_cast<uint8_t>(post);
          found = true;
        }
      }
    }
    if (!found) f.ok = false;
  }
  return f.ok;
}

namespace plain {

void Expand16(const Format16& f, const uint16_t* src, uint8_t* dst, unsigned n) {
  static const int slot[4] = {2, 1, 0, 3};  // r g b a -> B G R A
  for (unsigned i = 0; i < n; ++i) {
    const unsigned px = src[i];
    for (int c = 0; c < 4; ++c) {
      unsigned v;
      if (!f.bits[c]) {
        v = c == 3 ? 255 : 0;
      } else {
        const unsigned max = (1u << f.bits[c]) - 1;
        v = (((px & f.mask[c]) >> f.shift[c]) * 255 + max / 2) / max;
      }
      dst[i * 4 + slot[c]] = static_cast<uint8_t>(v);
    }
  }
}

void PackTo565(const uint8_t* src, uint16_t* dst, unsigned n) {
  for (unsigned i = 0; i < n; ++i, src += 4) {
    const unsigned b = (src[0] * 31u + 127) / 255, g = (src[1] * 63u + 127) / 255, r = (src[2] * 31u + 127) / 255;
    dst[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
  }
}

void ClearAlpha32(const uint32_t* src, uint32_t* dst, unsigned n) {
  for (unsigned i = 0; i < n; ++i) dst[i] = src[i] & 0x00FFFFFFu;
}

void SetAlpha32(const uint32_t* src, uint32_t* dst, unsigned n) {
  for (unsigned i = 0; i < n; ++i) dst[i] = src[i] | 0xFF000000u;
}

unsigned CountSoftAlpha(const uint8_t* bgra, unsigned n) {
  unsigned soft = 0;
  for (unsigned i = 0; i < n; ++i) {
    const uint8_t a = bgra[i * 4 + 3];
    soft += a > 8 && a < 247;
  }
  return soft;
}

unsigned AlphaKnown(const uint8_t* bgra, uint8_t* known, unsigned n) {
  unsigned unknown = 0;
  for (unsigned i = 0; i < n; ++i) {
    known[i] = bgra[i * 4 + 3] != 0;
    unknown += !known[i];
  }
  return unknown;
}

void FirstRing(const uint8_t* known, uint8_t* out, unsigned w, unsigned h, uint8_t* hd) {
  for (unsigned y = 0; y < h; ++y) {
    const uint8_t* k = known + static_cast<size_t>(y) * w;
    uint8_t* d = hd + static_cast<size_t>(y) * w;
    for (unsigned x = 0; x < w; ++x)
      d[x] = static_cast<uint8_t>((x ? k[x - 1] : 0) | k[x] | (x + 1 < w ? k[x + 1] : 0));
  }
  for (unsigned y = 0; y < h; ++y) {
    const uint8_t* up = y ? hd + static_cast<size_t>(y - 1) * w : nullptr;
    const uint8_t* mid = hd + static_cast<size_t>(y) * w;
    const uint8_t* dn = y + 1 < h ? hd + static_cast<size_t>(y + 1) * w : nullptr;
    const uint8_t* k = known + static_cast<size_t>(y) * w;
    uint8_t* o = out + static_cast<size_t>(y) * w;
    for (unsigned x = 0; x < w; ++x) {
      const uint8_t v = static_cast<uint8_t>(mid[x] | (up ? up[x] : 0) | (dn ? dn[x] : 0));
      o[x] = static_cast<uint8_t>(v & ~k[x] & 1);
    }
  }
}

void ConvertVertices(const VertexIn& in, const uint8_t* src, uint32_t count, void* out) {
  // Absent fields read a default instead, without stepping (no branches per field).
  const uint8_t* dp = in.diffuse >= 0 ? src + in.diffuse : reinterpret_cast<const uint8_t*>(&kWhite);
  const uint8_t* sp = in.specular >= 0 ? src + in.specular : reinterpret_cast<const uint8_t*>(kZeros);
  const uint8_t* t0 = in.t0 >= 0 ? src + in.t0 : reinterpret_cast<const uint8_t*>(kZeros);
  const uint8_t* t1 = in.t1 >= 0 ? src + in.t1 : reinterpret_cast<const uint8_t*>(kZeros);
  const uint32_t s = in.stride;
  const uint32_t ds = in.diffuse >= 0 ? s : 0, ss = in.specular >= 0 ? s : 0;
  const uint32_t t0s = in.t0 >= 0 ? s : 0, t1s = in.t1 >= 0 ? s : 0;
  // A one-float coordinate set: its second float is 0.
  const uint32_t v0 = in.t0 >= 0 && !in.t0_uv ? 0 : 0xFFFFFFFFu, v1 = in.t1 >= 0 && !in.t1_uv ? 0 : 0xFFFFFFFFu;
  uint32_t* o = static_cast<uint32_t*>(out);
  for (uint32_t i = 0; i < count; ++i, src += s, dp += ds, sp += ss, t0 += t0s, t1 += t1s, o += 10) {
    std::memcpy(o, src, 12);
    if (in.xyzw) std::memcpy(o + 3, src + 12, 4);
    else o[3] = 0x3F800000u;  // 1.0f
    std::memcpy(o + 4, dp, 4);
    std::memcpy(o + 5, sp, 4);
    std::memcpy(o + 6, t0, 4);
    uint32_t v;
    std::memcpy(&v, v0 ? t0 + 4 : t0, 4);
    o[7] = v & v0;
    std::memcpy(o + 8, t1, 4);
    std::memcpy(&v, v1 ? t1 + 4 : t1, 4);
    o[9] = v & v1;
  }
}

bool SamePositions(const uint8_t* verts, uint32_t stride, uint32_t count, const float* pos) {
  for (uint32_t i = 0; i < count; ++i)
    if (std::memcmp(verts + static_cast<size_t>(i) * stride, pos + i * 3, 12) != 0) return false;
  return true;
}

void SumPositions(const float* pos, uint32_t count, double sum[3]) {
  sum[0] = sum[1] = sum[2] = 0.0;
  for (uint32_t i = 0; i < count * 3; i += 3)
    for (int c = 0; c < 3; ++c) sum[c] += pos[i + c];
}

bool SumMoves(const float* prev, const float* cur, uint32_t count, float max2, double sum[3]) {
  sum[0] = sum[1] = sum[2] = 0.0;
  for (uint32_t i = 0; i < count * 3; i += 3) {
    const float dx = cur[i] - prev[i], dy = cur[i + 1] - prev[i + 1], dz = cur[i + 2] - prev[i + 2];
    if (dx * dx + dy * dy + dz * dz > max2) return false;
    sum[0] += dx;
    sum[1] += dy;
    sum[2] += dz;
  }
  return true;
}

double MaxBend(const float* prev, const float* cur, uint32_t count, const double mean[3]) {
  double bend = 0.0;
  for (uint32_t i = 0; i < count * 3; i += 3) {
    const double dx = cur[i] - prev[i] - mean[0], dy = cur[i + 1] - prev[i + 1] - mean[1],
                 dz = cur[i + 2] - prev[i + 2] - mean[2];
    const double d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > bend) bend = d2;
  }
  return bend;
}

void BlendPositions(const float* prev, const float* cur, uint32_t count, float alpha, uint8_t* verts,
                    uint32_t stride) {
  for (uint32_t i = 0; i < count; ++i) {
    float p[3];
    for (int c = 0; c < 3; ++c) p[c] = prev[i * 3 + c] + (cur[i * 3 + c] - prev[i * 3 + c]) * alpha;
    std::memcpy(verts + static_cast<size_t>(i) * stride, p, 12);
  }
}

}  // namespace plain
}  // namespace simd
