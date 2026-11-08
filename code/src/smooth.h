// Smooth drawing between fixed-rate simulation steps (see smooth.cpp).
#pragma once
#include <cstdint>

// The game set a world (type 1) or view (type 2) matrix for drawing. Writes
// the matrix to draw with into out (4x4, row vectors, like D3D7). `src` is the
// game's pointer to the matrix, used to tell objects apart between frames.
void SmoothTransform(uint32_t type, const float* in, const void* src, float* out);

// A frame was presented: start counting draws afresh.
void SmoothFrameEnd();

// Debug (SmoothLog=1): note a draw or vertex processing call for the trace.
void SmoothNoteDraw(const char* what, bool tl, unsigned verts);

// The game draws most objects (bikes, riders, trees) with vertices it placed
// in the world itself. Returns the vertices to draw: the game's, or a blend
// with the same draw one simulation step earlier (valid until the next call).
// Only the position (first 12 bytes of each vertex) is blended.
const uint8_t* SmoothVertices(const uint8_t* verts, uint32_t stride, uint32_t count,
                              const void* idx);

// Hook the game's GDI text so names over riders move with the blend.
void InstallTextSmoothing();

// Rebuild rider shadows from the blended rider each frame.
void InstallShadowSmoothing();

// Move the riders' projected-shadow cameras between steps.
void InstallProjectedShadowSmoothing();
