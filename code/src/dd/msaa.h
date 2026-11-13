// Multisample antialiasing (ini Antialiasing) for 3D scenes.
//
// The game draws into ordinary single-sample textures that it also reads
// back, blits and draws text on, so MSAA is kept to the side: while the
// device draws 3D into a render target that has a depth buffer, it draws
// into one multisampled colour + depth pair instead. The pair "mirrors" one
// render target at a time; anything else that touches that target's texture
// first resolves the MSAA copy back into it (see Surface::Gpu), and the next
// 3D draw copies the texture back into the MSAA buffer.
#pragma once
#include "backend.h"

namespace dd {

struct MsaaViews {
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  IDirect3DSurface9* rt9 = nullptr;  // Direct3D 9: multisampled colour + depth surfaces
  IDirect3DSurface9* ds9 = nullptr;
};

// Sample count in use (1 when off or unsupported).
UINT MsaaSamples();

// Route 3D drawing into `rt` through the MSAA buffers. Returns false when
// MSAA is off or unavailable, in which case the caller draws directly.
// `full_clear` tells it the caller is about to overwrite the whole colour
// buffer, so the current contents need not be copied in.
bool MsaaAcquire(GpuTexture* rt, MsaaViews& out, bool full_clear = false);
// The MSAA buffer was drawn into (it is now newer than the texture).
void MsaaMarkDrawn();
// Something other than 3D drawing is about to use `tex` (GpuTexture::Native):
// if it is the mirrored target, resolve pending MSAA drawing into it and stop
// mirroring.
void MsaaRelease(const void* tex);
// MotionBlur: if `tex` is the mirrored target, resolve the MSAA buffer into
// `dst` (a single-sample texture of the same size) without ending the
// mirroring. Returns false when `tex` is not mirrored.
bool MsaaResolveCopy(const void* tex, GpuTexture& dst);
// `tex` is being destroyed.
void MsaaForget(const void* tex);
// Direct3D 9 device reset: drop the buffers (they are recreated on demand).
void MsaaDeviceLost();

}  // namespace dd
