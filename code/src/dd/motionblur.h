// MotionBlur: speed-based blur of the race scene (see motionblur.cpp).
#pragma once
#include "../config.h"

namespace dd {

inline bool MotionBlurOn() { return GetConfig().motion_blur != 0; }

// From the bike simulation call (framerate.cpp), once per bike per step, on
// the game thread. Only reads the bike: the player's velocity is kept.
void MotionBlurBikeStep(const void* bike);

// Direct3D 9 device reset: drop the video-memory scene copy.
void MotionBlurDeviceLost();

}  // namespace dd
