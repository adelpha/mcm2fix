// Race HUD panel text (instrument overlay: position, lap, times), drawn by
// mcm2fix on screen instead of baked into the panel texture.
//
// The game draws each panel's text with GDI (DrawTextA/TextOutA, white) on a
// scratch texture, then a routine (0x4bafd0) copies the white pixels into
// the panel texture in the panel's colour; another (0x4bae60) wipes a
// rectangle of the panel back to its background. The panel is then drawn as
// a screen-space quad (overlay layout 0x4baad0, rectangle at +0xe8).
//
// With HudText=1 (and GpuText=1):
// - the text calls on the scratch texture are captured (not drawn there),
//   handed to the panel when the game converts those rows (with the colour
//   it asked for) and dropped when the game wipes that part of the panel;
// - each time the panel's quad is drawn, its text is drawn right over it by
//   GPU text, at screen resolution, scaled with the quad;
// - the text panels are laid out HudScale times larger (default 2).
#pragma once
#include <windows.h>

#include <cstdint>

namespace dd {

class Surface;

void InstallHudTextHooks();
// Surface::GetDC on a texture's CPU copy: true if it is a HUD text scratch
// texture (its text calls are then captured instead of drawn).
bool HudTextScratchDC(HDC dc, Surface* s, void* caller);
// A screen-space (pre-transformed) draw went to `rt`: if it was a text
// panel's quad, draw the panel's text over it.
void HudTextAfterDraw(Surface* rt, const uint8_t* verts, DWORD first, DWORD count, UINT stride);
// Start of every overlay layout (0x4baad0, hooked in fixes_widescreen.cpp):
// text panels get their rectangle scaled.
void HudPanelLayout(uintptr_t inst);

}  // namespace dd
