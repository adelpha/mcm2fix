// Settings from mcm2fix.ini next to the exe (all optional).
//
//   [mcm2fix]
//   OneCore=0     ; 1 = pin the game to one CPU core (old stopgap, default off)
//   Log=1         ; write %LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log
//   Startup=1     ; auto-pick the video card, skip the "detected a change" box
//   Registry=1    ; keep settings in HKCU instead of HKLM
//   Files=1       ; send file writes to %LOCALAPPDATA%\Motocross Madness 2
//   ActivateLog=1 ; log WM_ACTIVATEAPP
//   RegLog=1      ; log every registry call
//   DDLog=1       ; log DirectDraw / Direct3D 7 usage
#pragma once

struct Config {
  bool one_core = false;
  bool log = true;
  bool startup = true;
  bool registry = true;
  bool files = true;
  bool activate_log = true;
  bool reg_log = true;
  bool dd_log = true;
};

const Config& GetConfig();
void LoadConfig();
