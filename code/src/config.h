// Settings from mcm2fix.ini next to the exe (all optional; see README.md).
#pragma once

struct Config {
  bool one_core = false;      // OneCore: pin the game to one CPU core (old stopgap)
  bool log = true;            // Log: write logs\mcm2fix-<date>-<time>.log
  bool startup = true;        // Startup: auto-pick the video card, skip the hardware-change box
  bool registry = true;       // Registry: keep settings in HKCU instead of HKLM
  bool files = true;          // Files: send file writes to the per-user folder
  bool activate_log = false;  // ActivateLog: log window activation
  bool reg_log = false;       // RegLog: log every registry call
  bool profile = false;       // Profile: performance profiling (see prof.cpp)
  int profile_sample_us = 1000;  // ProfileSampleUs: sampling profiler interval (0 = off)
  bool fast_crt = true;
  bool simd = true;           // Simd: SSE2 fast paths for mcm2fix's own pixel and vertex work, when the CPU has SSE2 (simd.cpp)
  bool simd_log = false;      // SimdLog: time the SSE2 and plain paths at start-up, and log their use every 5 s
  bool dpi_aware = true;     // DpiAware: tell Windows the game is DPI aware (no scaling blur, no mouse drift) (fixes_startup.cpp)
  int race_power = 1;        // RacePower: 1 = High performance power plan while racing, 2 = while the game runs (power.cpp)
  bool fast_game_code = true;  // FastGameCode: badly compiled game loops rewritten in place, same results (fixes_perf.cpp)
  int async_sound = 2;        // AsyncSound: 3D sound listener commit on a worker thread; 2 = started at Present (fixes_perf.cpp)
  int sound_max_distance_mute = 1;  // SoundMaxDistanceMute: 1 = mute 3D sounds past their max distance ourselves (DSOAL ignores it), 2 = only log them (fixes_sound.cpp)
  char outdoor_reverb[6][32] = {};  // QuarryReverb/BajaReverb/NationalsReverb/SupercrossReverb/TagReverb/EnduroReverb: EAX 1.0 preset per event, by event number (Default = game's values) (fixes_reverb.cpp)
  bool background_mute = true;  // BackgroundMute: silence sound while the game is in the background (replacement DirectSound only) (fixes_sound.cpp)
  bool sound_log = false;     // SoundLog: log 3D sound buffers, mutes and unmutes
  bool sound_pause_log = false;  // SoundPauseLog: trace DirectSound calls and buffer states around race pause/unpause
  bool state_cache = true;    // StateCache: skip D3D11 state the device already has bound (device.cpp)       // FastCrt: QPC game clock and table-driven _stricmp (fixes_perf.cpp)
  bool draw_batch = true;     // DrawBatch: send 3D draws in a row with the same state as one draw (device.cpp)
  bool batch_log = false;     // BatchLog: log draw batching every 5 s
  bool draw_log = false;      // DrawLog: measure which draw calls could be merged (dd/drawlog.cpp)
  bool dd_stats = false;      // DDStats: log renderer activity every 2 seconds
  int renderer = 11;          // Renderer: 11 = Direct3D 11, 9 = Direct3D 9 (also used when D3D11 is missing)
  bool exclusive = false;     // Fullscreen: false = borderless window (default), true = exclusive fullscreen
  int fs_width = 0, fs_height = 0, fs_hz = 0;  // FullscreenMode: 0 = the desktop's
  bool renderer_label = true;  // RendererLabel: name the active renderer in the corner of the menus
  bool gpu_text = true;       // GpuText: draw the game's GDI text on the GPU at screen resolution
  float hud_scale = 2.0f;     // HudScale: race HUD text panels drawn this many times larger (1..4)
  float line_width = 3.0f;    // LineWidth: screen-space lines (speedometer needle) in pixels, antialiased (1 = original)
  float label_scale = 1.5f;   // LabelScale: name labels over the riders drawn this many times larger (1..4)
  char hud_font[260] = {};    // HudFont: .ttf file (relative to the game folder) for HUD panel and label text
  char hud_digit_font[260] = {};  // HudDigitFont: .ttf for the numbers in the stats panel (times, position, lap)
  float hud_stunt_text_size = 0.8f;  // HudStuntTextSize: stunt score panel text, relative to the race panel's
  float hud_digit_size = 0.7f;    // HudDigitSize: size of those numbers relative to the text around them
  bool race_title = true;     // RaceTitle: countdown race name intro in the middle of the screen (0 = game's corner text)
  float race_title_size = 1.0f;  // RaceTitleSize: size of that title (0.5..2)
  bool announcer = true;         // Announcer: countdown voices, air horn and final lap sounds from SOUNDS\ (when present)
  int announcer_volume = 100;     // AnnouncerVolume: their volume in percent (0..100)
  bool race_title_log = false;   // RaceTitleLog: trace the race title intro
  bool hud_text = true;       // HudText: race HUD panel text (GDI turned into textures) drawn sharp too
  bool text_log = false;      // TextLog: log GPU text drawing (first calls, what erases it)
  bool gdi_log = false;       // GdiLog: log the GDI calls the game draws text and boxes with
  bool d3d9_log = false;      // D3D9Log: extra logging from the Direct3D 9 renderer (first frames, fallbacks)
  bool wide_fov = true;       // WideFov: wide modes show more to the sides instead of cropping
  bool vsync = true;          // VSync: wait for the monitor's refresh when showing a frame
  int frame_cap = 0;          // FrameCap: most frames per second (0 = no cap beyond VSync)
  bool precise_timer = true;  // PreciseTimer: frame time from QueryPerformanceCounter, not whole ms
  bool frame_fix = true;      // FrameFix: per-frame animation/physics steps scale with frame time
  int ai_rate = 30;           // AiRate: computer riders re-plan this many times a second (0 = every frame)
  int smooth_log = 0;         // SmoothLog: 1 = trace a few frames of matrices and draws, 2 = camera only, 120 frames, 3 = rider shadows
  int sim_smooth = 1;         // SimSmooth: draw objects between simulation steps (needs SimRate)
  bool fast_sort = true;      // FastSort: replace the CRT qsort (quadratic on equal keys) with a merge sort (fixes_sort.cpp)
  int tree_cache_mb = 32;     // TreeCacheMB: trim the vegetation cache only when this much is unused (0 = original 256 KB)
  bool sort_log = false;      // SortLog: sort calls per caller every 10 s
  bool input_timing = true;   // InputTiming: race steps get the input of their own time slice; taps held one step (input_timing.cpp)
  bool alt_tab_fix = true;    // AltTabFix: keys let go while alt-tabbed are released; lost devices acquired again (input_timing.cpp)
  bool input_log = false;     // InputLog: log input timing (events per step, held taps, joystick samples)
  int sim_rate = 30;          // SimRate: race simulation steps per second, drawing stays per frame (0 = one step per frame)
  int ai_accel[3] = {80, 130, 150};      // AiAccelEasy/Normal/Hard: computer rider acceleration, percent of the base curve (fixes_ai.cpp)
  int ai_top_speed[3] = {90, 100, 100};  // AiTopSpeedEasy/Normal/Hard: computer rider top speed, percent
  bool ai_pick_fix = true;                // AiPickFix: computer riders' random bike/rider picks can choose the last one too
  bool ai_log = false;                    // AiLog: log computer rider speeds, jumps and respawns
  int ai_steer_rate = 0;    // AiSteerRate: fastest a computer rider's front wheel turns, degrees/s (0 = no limit)
  int ref_rate = 30;          // RefRate: frame rate those per-frame steps were tuned for
  bool arrow_heading = true;  // ArrowFix: draw the race direction arrow without perspective lean
  float arrow_turn = 0.8f;    // ArrowTurn: how much of that correction to apply (1 = full)
  bool fov_log = false;       // FovLog: every camera field-of-view change (who set it, what we widened it to)
  bool arrow_log = false;     // ArrowLog: trace the direction arrow and camera for 4 s after each race start
  float arrow_tilt = 10.0f;   // ArrowTilt: degrees the arrow's nose is raised, to see it more from above
  float arrow_scale = 2.0f;   // ArrowScale: size of the race direction arrow (1 = original)
  bool restart_fix = true;   // RestartFix: Restart Race turns the bikes to face the course again
  bool restart_log = false;  // RestartLog: log the start grid at race load and Restart Race
  bool alpha_log = false;    // AlphaLog: log each blended/alpha-tested draw combination and whether AlphaCut applied
  int alpha_cut = 128;        // AlphaCut: alpha-tested blended pixels below this (0-255) are dropped
  int alpha_to_coverage = 2;  // AlphaToCoverage: cut-out edges via MSAA coverage (0 off, 1 sharp, 2 soft)
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
  bool true_color_textures = true;  // TrueColorTextures: load textures as 24/32-bit instead of 16-bit
  bool texture_log = false;         // TextureLog: log the format of each texture the game creates
};

const Config& GetConfig();
void LoadConfig();
