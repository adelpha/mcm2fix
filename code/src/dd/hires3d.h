// MenuHiRes3D=1: 3D drawn in the menus (the bike and rider in the garage on
// the Bike & Rider page) at the window's resolution instead of 640x480.
//
// The menus are a 640x480 surface that is scaled up when presented. While
// the device draws 3D into such a surface (one with a depth buffer), every
// draw and clear is made a second time into a multisampled colour + depth
// pair of the window's size. When anything else touches the surface (a
// blit, a 2D draw, a readback, presenting), the area the 3D went into is
// resolved and put, opaque, into the surface's GPU text overlay (gputext.h),
// which is already kept at the window's resolution, follows the surface
// through flips and copies, is erased by whatever the game draws over it
// later, and is blended over the surface when it is shown. The 640x480
// drawing still happens as before underneath, so readbacks are unchanged.
//
// Direct3D 11 only, needs GpuText=1. Off in races (the race is drawn at its
// own resolution anyway).
#pragma once
#include <windows.h>

#include "backend.h"

namespace dd {

// 3D drawing into `rt` (w x h, with a depth buffer) is about to happen:
// true if it should also go to the window-sized copy (buffers are ready).
bool Hi3dPrepare(const GpuTexture& rt, UINT w, UINT h);
// The views and scale (copy pixels per surface pixel) for the draw just
// prepared.
bool Hi3dViews(ID3D11RenderTargetView** rtv, ID3D11DepthStencilView** dsv, float* sx, float* sy, UINT* w, UINT* h);
// r (surface pixels) of the prepared target was drawn or cleared.
void Hi3dDrawn(const RECT& r, bool clear);
// Something other than 3D drawing uses texture `native`: if it is the one
// being mirrored, put what was drawn into its overlay.
void Hi3dRelease(const void* native);
void Hi3dForget(const void* native);
// A race started (countdown) / ended (BikeRace destroyed).
void Hi3dRaceLive(bool live);

}  // namespace dd
