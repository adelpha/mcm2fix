// Settings from mcm2fix.ini next to the exe (all optional).
//
//   [mcm2fix]
//   OneCore=1     ; pin the game to one CPU core (stopgap, default on)
//   Log=1         ; write %LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log
#pragma once

struct Config {
  bool one_core = true;
  bool log = true;
};

const Config& GetConfig();
void LoadConfig();
