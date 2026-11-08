// Settings from mcm2fix.ini next to the exe (all optional; see README.md).
#pragma once

struct Config {
  bool one_core = false;      // OneCore: pin the game to one CPU core (old stopgap)
  bool log = true;            // Log: write mcm2fix.log
  bool startup = true;        // Startup: auto-pick the video card, skip the hardware-change box
  bool registry = true;       // Registry: keep settings in HKCU instead of HKLM
  bool files = true;          // Files: send file writes to the per-user folder
  bool activate_log = false;  // ActivateLog: log window activation
  bool reg_log = false;       // RegLog: log every registry call
  bool dd_stats = false;      // DDStats: log renderer activity every 2 seconds
  bool wide_fov = true;       // WideFov: wide modes show more to the sides instead of cropping
  bool arrow_heading = true;  // ArrowFix: draw the race direction arrow without perspective lean
  float arrow_turn = 0.8f;    // ArrowTurn: how much of that correction to apply (1 = full)
  float arrow_scale = 2.0f;   // ArrowScale: size of the race direction arrow (1 = original)
  int alpha_cut = 128;        // AlphaCut: alpha-tested blended pixels below this (0-255) are dropped
};

const Config& GetConfig();
void LoadConfig();
