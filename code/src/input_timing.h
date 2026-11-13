// Input timing: each race simulation step gets the input of its own moment
// (see input_timing.cpp).
#pragma once
#include <windows.h>

namespace input {

// Hook DirectInput (InputTiming in mcm2fix.ini). Call once at startup.
void Apply();

// Before each simulation step. now = QPC at the start of the frame's tick,
// behind = seconds of real time the end of this step lies before now (0 for
// the frame's last step), index/count = this step's place in the frame's
// steps, racing = a race is running at the fixed simulation rate.
void BeginStep(LONGLONG now, double behind, int index, int count, bool racing);

// The game window became active or inactive (WM_ACTIVATEAPP). Inactive:
// everything the game thinks is held gets a release (AltTabFix); active
// again: each device's state is checked at its next read.
void OnActivate(bool active);

// After a frame's steps: input read outside steps (menus) sees everything.
void EndSteps();

}  // namespace input
