// SSE2 fast paths for mcm2fix's own per-frame work (ini Simd), picked at run
// time from the CPU, so the DLL still runs on a Pentium II or Athlon without
// SSE2. Only mcm2fix's own code: the game's physics and other game code keep
// their x87 maths untouched.
//
// Every kernel has a plain version (simd_plain.cpp) and an SSE2 version
// (simd_sse2.cpp). Pixel and vertex copies give exactly the same bytes;
// the smoothing maths the same within float rounding. The start-up self
// test checks that before the SSE2 paths are used (SimdLog also times them
// against each other); tests/simd_test.cpp checks it exhaustively.
#pragma once
#include <cstdint>

namespace simd {

// Detect the CPU, read Simd/SimdLog, self-test and log (once, at start-up;
// also run on first use).
void Init();
// True when the SSE2 paths are in use.
bool Sse2();
// "SSE2" or "plain".
const char* PathName();
// SimdLog=1: per-kernel calls, items and time every 5 seconds.
void ReportStats();

// ---- 16-bit pixels to B8G8R8A8 --------------------------------------------
// A 16-bit pixel format as masks (r, g, b, a; a may be 0 = opaque). Each
// channel is widened to 8 bits exactly like PixelLayout::ToBGRA
// ((v * 255 + max / 2) / max).
struct Format16 {
  uint32_t mask[4];
  // Filled by Prepare16: shift, bits, and the multiply/shift that does the
  // rounding divide by (2^bits - 1) (mul 0 for one bit: no divide).
  uint8_t shift[4], bits[4];
  uint16_t mul[4];
  uint8_t post[4];
  bool ok;
};
bool Prepare16(Format16& f, const uint32_t mask[4]);
void Expand16(const Format16& f, const uint16_t* src, uint8_t* dst, unsigned n);

// ---- B8G8R8A8 to the surface format (readback) -----------------------------
// R5G6B5, rounding like PixelLayout::FromBGRA ((v * max + 127) / 255).
void PackTo565(const uint8_t* src, uint16_t* dst, unsigned n);
// X8R8G8B8: alpha byte cleared.
void ClearAlpha32(const uint32_t* src, uint32_t* dst, unsigned n);
// B8G8R8X8 to opaque B8G8R8A8 (alpha byte set to 255).
void SetAlpha32(const uint32_t* src, uint32_t* dst, unsigned n);

// ---- Alpha statistics on B8G8R8A8 -------------------------------------------
// Texels with alpha in 9..246 (partly transparent).
unsigned CountSoftAlpha(const uint8_t* bgra, unsigned n);
// known[i] = alpha != 0; returns the number of texels with alpha 0.
unsigned AlphaKnown(const uint8_t* bgra, uint8_t* known, unsigned n);
// For a w x h map of 0/1 bytes: out[i] = 1 where known[i] is 0 and one of
// its 8 neighbours is 1 (the first ring of transparent texels to fill).
void FirstRing(const uint8_t* known, uint8_t* out, unsigned w, unsigned h);

// ---- Vertices ---------------------------------------------------------------
// Our 40-byte GPU vertex: x y z w, diffuse, specular, uv0, uv1.
struct VertexIn {
  uint32_t stride;
  bool xyzw;       // copy 4 floats (screen space); else 3 and w = 1
  int diffuse;     // byte offsets in the game's vertex, -1 = absent
  int specular;    //   (white / black)
  int t0, t1;      // texture coordinate sets, -1 = absent (0, 0)
  bool t0_uv, t1_uv;  // two floats; else one (the second is 0)
};
void ConvertVertices(const VertexIn& in, const uint8_t* src, uint32_t count, void* out);

// Positions (first 12 bytes of each vertex) into a packed x y z array, and
// back; compares them with a packed array (bitwise, like memcmp).
void GatherPositions(const uint8_t* verts, uint32_t stride, uint32_t count, float* pos);
bool SamePositions(const uint8_t* verts, uint32_t stride, uint32_t count, const float* pos);
// Sum of the positions (double, for the centre).
void SumPositions(const float* pos, uint32_t count, double sum[3]);
// Move from prev to cur of each point: false if any moved more than
// sqrt(max2); else the summed move.
bool SumMoves(const float* prev, const float* cur, uint32_t count, float max2, double sum[3]);
// Largest squared distance of a point's move from `mean`.
double MaxBend(const float* prev, const float* cur, uint32_t count, const double mean[3]);
// Writes prev + (cur - prev) * alpha into each vertex's position.
void BlendPositions(const float* prev, const float* cur, uint32_t count, float alpha, uint8_t* verts,
                    uint32_t stride);

}  // namespace simd
