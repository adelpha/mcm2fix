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
  int alpha_cut = 128;
  bool vsync = true;          // VSync: wait for the monitor's refresh when showing a frame
  int frame_cap = 0;          // FrameCap: most frames per second (0 = no cap beyond VSync)
  bool precise_timer = true;  // PreciseTimer: frame time from QueryPerformanceCounter, not whole ms
  bool frame_fix = true;      // FrameFix: per-frame animation/physics steps scale with frame time
  int ai_rate = 30;           // AiRate: computer riders re-plan this many times a second (0 = every frame)
  bool smooth_log = false;    // SmoothLog: trace a few frames of matrices and draws
  int sim_smooth = 0;         // SimSmooth: draw objects between simulation steps (needs SimRate)
  int sim_rate = 30;          // SimRate: race simulation steps per second, drawing stays per frame (0 = one step per frame)
  int ai_steer_rate = 0;    // AiSteerRate: fastest a computer rider's front wheel turns, degrees/s (0 = no limit)
  int ref_rate = 30;          // RefRate: frame rate those per-frame steps were tuned for        // AlphaCut: alpha-tested blended pixels below this (0-255) are dropped
};

const Config& GetConfig();
void LoadConfig();
