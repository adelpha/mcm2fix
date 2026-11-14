// MenuFlag=1: a waving dark blue chequered flag behind the main menu, under
// a replacement background picture with transparency (MenuBackground).
//
// The game's own background (01_Main\BackGround_01.tga in UI\MAINMENU.DAT)
// is swapped for a flat key-green picture (menus.cpp). Wherever the menu
// frame still shows that green, Present draws the flag and the picture
// instead; everything the game draws on top (buttons, chain animations,
// text, the cursor) stays. Direct3D 11 only.
#pragma once
#include <windows.h>

#include "backend.h"

namespace dd {

// MenuFlag on, Direct3D 11 running and the picture loads (decoded once).
// Asked when the game opens UI\MAINMENU.DAT.
bool MenuBgWanted();

// The 16-bit key colour (X1R5G5B5) the replaced background is filled with.
constexpr unsigned short kMenuBgKey555 = 0x03E0;  // pure green

// Present: draws the 640x480 menu frame into the swap chain with the flag
// and picture under its key-green pixels. False = not handled (draw as usual).
bool MenuBgPresent(ID3D11ShaderResourceView* frame, UINT frame_w, UINT frame_h, const RECT& game_rect);

}  // namespace dd
