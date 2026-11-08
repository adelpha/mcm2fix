// Frame pacing: frame-rate cap and precise frame time (see framerate.cpp).
#pragma once

// Patch the game's frame timer and set up the cap. Call once at startup.
void ApplyFramePacing();

// Block until the frame cap (FrameCap in mcm2fix.ini) allows another frame.
// Call without holding the renderer lock.
void WaitForFrameSlot();
