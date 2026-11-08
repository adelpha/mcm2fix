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
// Something other than 3D drawing is about to use `tex`: if it is the
// mirrored target, resolve pending MSAA drawing into it and stop mirroring.
void MsaaRelease(ID3D11Texture2D* tex);
// `tex` is being destroyed.
void MsaaForget(ID3D11Texture2D* tex);

}  // namespace dd
