// Rider shadows drawn by the graphics card (ini RiderShadows, Direct3D 11).
//
// The game's own shadow (ProjectedShadow, vtable 0x55c848) fits one window
// around the rider and bike the camera follows, draws them into a 128x128
// 16-bit texture on the CPU (0x4e00b0, a software triangle filler), then a
// receiver (vtable 0x55d658) cuts the terrain triangles under that window
// out on the CPU and draws them again with the texture, darkening the
// ground. Only that one rider has a shadow.
//
// With RiderShadows on, none of that runs. Instead, once per frame (at the
// game's shadow pre-draw, after the riders were placed between simulation
// steps by SimSmooth):
//  - every rider's and bike's world-space mesh is drawn on the GPU into its
//    own tile of a shadow atlas (an orthographic view from the sun, 4x MSAA
//    resolved to soft-edged coverage);
//  - the terrain draws (the game's patch draws 0x481c00 / 0x481f20) get a
//    pixel shader variant that looks each pixel up in those tiles and
//    darkens it, before fog.
// Direct3D 9 (fixed function, no shaders) keeps the game's own shadow.
#pragma once
#include <windows.h>
#include <d3d11.h>

#include <cstdint>

namespace dd {

// Install the hooks (after the smoothing and restart hooks, which it chains).
void InstallGpuShadows();

// True while the game draws a terrain patch (shadow receivers).
bool ShadowReceiverNow();

// For a terrain draw on the Direct3D 11 device: if GPU shadows are on this
// frame, bind the atlas and constants (slots b2, t3, s3) for `world` (the
// draw's world matrix, row vectors) and return true; the caller then uses
// the shadow shader variant. `blend`, `src`, `dst`: the draw's blend state
// (multiplying layers over shadowed ground are left alone).
bool ShadowBindReceiver(const float* world, bool blend, DWORD src, DWORD dst);

// Presented a frame (surface flip): per-frame bookkeeping and the ShadowLog
// summary.
void ShadowFrameEnd();

}  // namespace dd
