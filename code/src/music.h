// Optional music for the menus and races ([Music] section of mcm2fix.ini).
#pragma once

#include <cstdint>

namespace music {

// True when any music is set in the ini (reads the ini on first call).
bool Enabled();
// DllMain: reads the settings, watches the game opening track files.
void Apply();
// Every frame from the game thread (just before Present).
void Frame();
// The game window became active / inactive (WM_ACTIVATEAPP).
void OnActivate(bool active);
// A race's countdown was set (start or Restart Race) / the race is destroyed.
void RaceStart(uintptr_t race);
void RaceEnd(uintptr_t race);
// A game video started (true) or stopped (false) playing.
void VideoPlaying(bool playing);
// The game opened a video file (the menus' first one ends the intro).
void VideoOpened(const char* path);
// The game set a display mode (menus are 640x480; a race's mode = loading).
void DisplayMode(unsigned w, unsigned h);
// The player crossed the finish line (finish text queued).
void RaceFinished();

}  // namespace music
