// One-shot 2D sounds from the game folder's SOUNDS\ (race announcer),
// played through waveOut alongside the game's own sound.
#pragma once

namespace dd {

// Load SOUNDS\<file> now, so playing it later does not stall a frame.
void OneShotPreload(const char* file);
// Play SOUNDS\<file> once (false: missing, unsupported or failed; logged).
bool OneShotPlay(const char* file, const char* why);
// Close finished sounds.
void OneShotTick();

}  // namespace dd
