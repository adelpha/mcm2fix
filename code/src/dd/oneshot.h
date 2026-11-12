// One-shot 2D sounds from the game folder's SOUNDS\ (race announcer),
// played through waveOut alongside the game's own sound.
#pragma once

namespace dd {

// All waveOut work runs on a sound thread; these only queue a request.
// Load SOUNDS\<file> ahead of time.
void OneShotPreload(const char* file);
// Play SOUNDS\<file> once (missing, unsupported or failed: logged, skipped).
void OneShotPlay(const char* file, const char* why);
// Close finished sounds.
void OneShotTick();

}  // namespace dd
