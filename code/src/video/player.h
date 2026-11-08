// Replaces the game's DirectShow video player (see player.cpp).
#pragma once

namespace video {

void ApplyVideoPlayer();
// True for the stream samples our player hands to the game.
bool IsOwnSample(const void* sample);

}  // namespace video
