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

// Blend each bike/rider part's matrix between steps before the game builds
// the object's vertices from it (smooth_parts.cpp).
void InstallPartSmoothing();
// Around the game's Draw: rebuild objects from blended parts, then mark them
// for rebuilding from the step afterwards.
void SmoothDrawBegin();
void SmoothDrawEnd();
// True if `verts` were filled from blended parts this frame.
bool SmoothPartsOwn(const void* verts);
// True while drawing with blended parts.
bool SmoothPartsActive();

// Blend the screen position of the names over riders between steps.
void InstallNameSmoothing();

// Smoothing cull margin (fixes_widescreen.cpp): the scale to apply to a
// projection matrix the game sets (1 if it isn't a widened camera's), and
// the ratio for a camera's screen positions.
float SmoothProjectionScale(const float* m);
bool SmoothCullMargin(uintptr_t cam, float* ratio);
unsigned SmoothMarginHits();

// Frame end bookkeeping for smooth_parts.cpp; `log` every 600 frames.
void PartsFrameEnd(bool log);
