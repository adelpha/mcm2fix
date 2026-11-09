// GPU text (ini GpuText=1): the game's GDI drawing on render targets
// (TextOutA, DrawTextA, FillRect, FrameRect, LineTo; see gdihook.cpp) is
// drawn by mcm2fix instead, at the resolution the frame is shown at, into a
// transparent "text overlay" that belongs to the surface's GPU texture.
// Glyphs come from an antialiased glyph atlas (GetGlyphOutline, GGO_GRAY8).
//
// The overlay follows the texture through flips. Anything the game later
// draws over the same pixels (Blt, colour fill, Clear, 3D draws, CPU writes)
// erases the overlay there, so the result stacks like the original. When the
// surface is presented, the overlay is blended over it.
#pragma once
#include <windows.h>

#include "backend.h"

namespace dd {

bool GpuTextActive();
// A device context the game's GDI calls are recorded from, for the surface
// whose GPU copy is `base` (w x h). Null if GPU text cannot be used.
HDC GpuTextBeginDC(const GpuTexture& base, UINT w, UINT h);
void GpuTextEndDC(HDC dc);
// Record-only: GDI still draws on `dc` (a texture's CPU copy), and its text
// is also drawn into an overlay keyed by `key` at GpuTextTextureScale().
// GpuTextEndDC ends it. false: not recording.
bool GpuTextRecordDC(HDC dc, const void* key, UINT w, UINT h);
float GpuTextTextureScale();
// Blend the overlay text of r (surface pixels) of `key`, tinted rgba
// (0..1, straight), over the same place in `dst`, a copy of the surface
// made at any size. false: no text there.
bool GpuTextBake(const void* key, const RECT& r, const GpuTexture& dst, const float rgba[4]);

// The game drew over r (surface pixels; null = everything) of `native`.
void GpuTextOcclude(const void* native, const RECT* r, const char* why);
bool GpuTextHasText(const void* native);
// The game copied sr of `src` to dr of `dst` (dst_w x dst_h): take the text
// along (after the destination's own text there was erased). `over`: blend
// it over instead of replacing (colour-keyed copies).
void GpuTextCopy(const void* src, const RECT& sr, const void* dst, UINT dst_w, UINT dst_h, const RECT& dr, bool over);
// A colour-keyed sprite was blitted onto `native`: erase the overlay only
// where the sprite is opaque (`sprite` is the blit as it was made).
void GpuTextOccludeSprite(const void* native, const BlitParams& sprite);
// Blend the overlay of `src` (if any) over r of the destination.
void GpuTextComposite(const GpuTexture& src, ID3D11RenderTargetView* dst, IDirect3DSurface9* dst9, UINT dst_w,
                      UINT dst_h, const RECT& r);
// The texture is going away.
void GpuTextForget(const void* native);
// Direct3D 9 device reset: overlays live in D3DPOOL_DEFAULT.
void GpuTextDeviceLost();

}  // namespace dd
