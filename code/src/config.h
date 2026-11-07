// Settings from mcm2fix.ini next to the exe (all optional).
//
//   [mcm2fix]
//   OneCore=1     ; pin the game to one CPU core (stopgap, default on)
//   Log=1         ; write %LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log
//   Startup=1     ; auto-pick the video card, skip the "detected a change" box
//   Registry=1    ; keep settings in HKCU instead of HKLM
//   Files=1       ; send file writes to %LOCALAPPDATA%\Motocross Madness 2
//   ActivateLog=1 ; log WM_ACTIVATEAPP
#pragma once

struct Config {
  bool one_core = true;
  bool log = true;
  bool startup = true;
  bool registry = true;
  bool files = true;
  bool activate_log = true;
};

const Config& GetConfig();
void LoadConfig();
