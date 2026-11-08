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
