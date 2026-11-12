// Race title intro (ini RaceTitle=1): during the countdown, the event's name
// and lap count (stunts: time limit) are shown in the middle of the screen,
// opening out of a white line and closing back into it as the countdown
// ends, instead of the game's small green text in the bottom-left corner.
#pragma once

namespace dd {

class Surface;

void InstallRaceTitleHooks();
// A frame was shown: the title is drawn again at the next end of scene.
void RaceTitleNewFrame();
// The game ended a scene on `rt`: draw the title over it (once per frame).
void RaceTitleDraw(Surface* rt);

}  // namespace dd
