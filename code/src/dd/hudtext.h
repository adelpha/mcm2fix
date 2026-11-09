// Antialiased text on the race HUD panels (GPU text for textures).
//
// The race HUD (instrument overlay: lap, position, times) is drawn by the game
// in two steps: white GDI text on a scratch texture, then a routine (0x4bafd0)
// that copies every white pixel into the panel texture in the panel's colour.
// Another routine (0x4bae60) wipes a rectangle of the panel back to its
// background before new text goes in.
//
// With GpuText=1, the GDI text on the scratch texture is also recorded at
// screen resolution (gputext.cpp, record-only DCs), and the panel texture gets
// a sharper copy at screen resolution that the 3D draws sample instead
// (Surface::Hires). Both routines are hooked at their call sites to keep that
// copy in step: the wipe is copied over from the panel, and the converted
// text is drawn with antialiased glyphs in the colour the game used.
#pragma once
#include <windows.h>

namespace dd {

class Surface;

void InstallHudTextHooks();
// A blit from a panel with a sharper copy: the copy goes along to the
// destination texture once the blit is done (the game copies its panels
// into the video-memory textures it draws with).
class HiresCarry {
 public:
  HiresCarry(Surface* src, const RECT& sr, Surface* dst, const RECT& dr, bool key);
  ~HiresCarry();

 private:
  Surface* src_;
  Surface* dst_;
  RECT sr_, dr_;
  bool key_;
  bool dst_current_ = false;
};
// The 3D pipeline sampled a sharper copy (for the log's counts).
void NoteHiresSample();
// A texture with a sharp copy was drawn in screen space, `scale` screen
// pixels per texel: the copies are remade at that scale.
void NoteHudDraw(Surface* tex, float scale);

}  // namespace dd
