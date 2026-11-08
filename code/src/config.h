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
  float arrow_tilt = 10.0f;   // ArrowTilt: degrees the arrow's nose is raised, to see it more from above
  float arrow_scale = 2.0f;   // ArrowScale: size of the race direction arrow (1 = original)
  int alpha_cut = 128;        // AlphaCut: alpha-tested blended pixels below this (0-255) are dropped
  // Graphics options.
  float fov = 0.0f;           // Fov: camera field of view in degrees at 4:3 (0 = game's own, 77)
  int anisotropy = 16;        // Anisotropy: anisotropic filtering level (0/1 = off, up to 16)
  int msaa = 4;               // Antialiasing: MSAA samples for 3D scenes (0/1 = off, 2, 4, 8)
  float mip_bias = 0.0f;      // MipBias: texture mip level bias (negative = sharper at a distance)
  float fog_scale = 1.0f;     // FogDistance: multiplies the game's fog distances
  float view_distance = 1.0f; // ViewDistance: multiplies the game's view (far clip) distance
  bool tree_always_near = false;  // TreeDetailDistance=0: trees always use their close-up version
  float model_lod = 1.0f;     // ModelLod: detailed models are kept this many times further away
  bool texture_replace = true;  // TextureReplace: load replacement textures from textures\ folder
  bool texture_dump = false;    // TextureDump: save every texture the game uploads as PNG
};

const Config& GetConfig();
void LoadConfig();
