// The two sets of kernels behind simd.h: plain (simd.cpp) and SSE2
// (simd_sse2.cpp). Internal to simd*.cpp.
//
// simd_sse2.cpp is compiled with -msse2 (GCC): it must not include headers
// with inline functions or templates (the C++ library), or the linker could
// keep its SSE2 copy of them for the whole DLL. This header has none.
#pragma once
#include "simd.h"

namespace simd {

#define MCM2FIX_SIMD_KERNELS(NS)                                                                       \
  namespace NS {                                                                                       \
  void Expand16(const Format16& f, const uint16_t* src, uint8_t* dst, unsigned n);                     \
  void PackTo565(const uint8_t* src, uint16_t* dst, unsigned n);                                       \
  void ClearAlpha32(const uint32_t* src, uint32_t* dst, unsigned n);                                   \
  void SetAlpha32(const uint32_t* src, uint32_t* dst, unsigned n);                                     \
  unsigned CountSoftAlpha(const uint8_t* bgra, unsigned n);                                            \
  unsigned AlphaKnown(const uint8_t* bgra, uint8_t* known, unsigned n);                                \
  void FirstRing(const uint8_t* known, uint8_t* out, unsigned w, unsigned h, uint8_t* scratch);       \
  void ConvertVertices(const VertexIn& in, const uint8_t* src, uint32_t count, void* out);             \
  bool SamePositions(const uint8_t* verts, uint32_t stride, uint32_t count, const float* pos);         \
  void SumPositions(const float* pos, uint32_t count, double sum[3]);                                  \
  bool SumMoves(const float* prev, const float* cur, uint32_t count, float max2, double sum[3]);        \
  double MaxBend(const float* prev, const float* cur, uint32_t count, const double mean[3]);           \
  void BlendPositions(const float* prev, const float* cur, uint32_t count, float alpha, uint8_t* verts, \
                      uint32_t stride);                                                                \
  }

MCM2FIX_SIMD_KERNELS(plain)
MCM2FIX_SIMD_KERNELS(sse2)

// Defaults for vertex fields the game's format lacks.
extern const uint32_t kWhite;
extern const uint32_t kZeros[4];

}  // namespace simd
