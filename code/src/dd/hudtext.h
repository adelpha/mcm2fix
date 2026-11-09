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

namespace dd {

void InstallHudTextHooks();
// The 3D pipeline sampled a sharper copy (for the log's counts).
void NoteHiresSample();

}  // namespace dd
