// Settings from mcm2max.ini next to the exe (all optional; see README.md).
#pragma once
#include <cstdint>
#include <string>

struct Config {
  bool one_core = false;      // OneCore: pin the game to one CPU core (old stopgap)
  bool log = true;            // Log: write logs\MCM2MaX-<date>-<time>.log
  bool startup = true;        // Startup: auto-pick the video card, skip the hardware-change box
  bool registry = true;       // Registry: keep settings in HKCU instead of HKLM
  bool files = true;          // Files: send file writes to the per-user folder
  bool activate_log = false;  // ActivateLog: log window activation
  bool reg_log = false;       // RegLog: log every registry call
  bool profile = false;       // Profile: performance profiling (see prof.cpp)
  int profile_sample_us = 1000;  // ProfileSampleUs: sampling profiler interval (0 = off)
  bool fast_crt = true;
  bool fast_ftol = true;      // FastFtol: the C runtime's float-to-integer call done with one instruction (SSE3)
  bool simd = true;           // Simd: SSE2 fast paths for MCM2MaX's own pixel and vertex work, when the CPU has SSE2 (simd.cpp)
  bool simd_log = false;      // SimdLog: time the SSE2 and plain paths at start-up, and log their use every 5 s
  bool pause_on_focus_loss = true;  // PauseOnFocusLoss: alt-tabbing out of a race opens the pause menu (diagnostics.cpp)
  bool dpi_aware = true;     // DpiAware: tell Windows the game is DPI aware (no scaling blur, no mouse drift) (fixes_startup.cpp)
  int race_power = 1;        // RacePower: 1 = High performance power plan while racing, 2 = while the game runs (power.cpp)
  int race_power_async = 1;  // RacePowerAsync: 1 = power plan calls on their own thread (power.cpp)
  bool fast_game_code = true;  // FastGameCode: badly compiled game loops rewritten in place, same results (fixes_perf.cpp)
  int async_sound = 2;        // AsyncSound: 3D sound listener commit on a worker thread; 2 = started at Present (fixes_perf.cpp)
  int sound_max_distance_mute = 1;  // SoundMaxDistanceMute: 1 = mute 3D sounds past their max distance ourselves (DSOAL ignores it), 2 = only log them (fixes_sound.cpp)
  char outdoor_reverb[6][32] = {};  // QuarryReverb/BajaReverb/NationalsReverb/SupercrossReverb/TagReverb/EnduroReverb: EAX 1.0 preset per event, by event number (Default = game's values) (fixes_reverb.cpp)
  bool background_mute = true;  // BackgroundMute: silence sound while the game is in the background (replacement DirectSound only) (fixes_sound.cpp)
  bool sound_log = false;     // SoundLog: log 3D sound buffers, mutes and unmutes
  bool sound_pause_log = false;  // SoundPauseLog: trace DirectSound calls and buffer states around race pause/unpause
  bool state_cache = true;    // StateCache: skip D3D11 state the device already has bound (device.cpp)       // FastCrt: QPC game clock and table-driven _stricmp (fixes_perf.cpp)
  bool still_scenery = true;  // StillScenery: terrain and tree draws skip the SimSmooth vertex checks (device.cpp)
  bool fast_object_walk = true;  // FastObjectWalk: the scene's object passes walk a flat list (fixes_walk.cpp)
  bool walk_log = false;      // WalkLog: FastObjectWalk statistics every 5 s
  bool far_trees = true;      // FarTrees: far tree billboards drawn on the GPU (fartrees.cpp)
  bool far_tree_cells = true;  // FarTreeCells: grid cells holding only far trees are left out of the walk
  float far_tree_distance = 0;  // FarTreeDistance: from how far (0 = automatic)
  bool terrain_lod_fix = true;  // TerrainLodFix: clamp the far-block detail number (fixes_terrain.cpp)
  bool terrain_hole_fix = true;  // TerrainHoleFix: refresh children of wholly-visible split nodes (fixes_terrain.cpp)
  float terrain_detail = 2.0f;  // TerrainDetail: terrain mesh detail multiplier (fixes_terrain.cpp)
  bool fast_terrain = true;   // FastTerrain: exact rewrites of the per-frame terrain code (terrain_fast.cpp)
  int terrain_check = 0;      // TerrainCheck: compare FastTerrain with the game's own code every 2 s
  int terrain_mesh_cache_kb = 32768;  // TerrainMeshCache: KB of terrain meshes kept when not drawn (game: 1024)
  bool gpu_terrain = true;    // GpuTerrain: whole terrain built once at full detail on the GPU (gputerrain.cpp)
  bool gpu_terrain_detail = true;
  int gpu_terrain_far = 1;  // GpuTerrainFar: far terrain coloured and without detail, as the game's far blocks
  float gpu_terrain_detail_reach = 0;  // GpuTerrainDetailReach: how far the detail texture reaches, 0 = the game's
  int free_cam = 1;             // FreeCam: Ctrl+F6 in a race freezes it and gives a fly camera
  float free_cam_speed = 60.0f; // FreeCamSpeed: feet per second (Shift x5, Z x0.2)
  int free_cam_mouse = 15;      // FreeCamMouse: mouse look sensitivity
  int gpu_terrain_log = 1;    // GpuTerrainLog: 0 off, 1 build and 5 s lines, 2 also tint by node level
  int terrain_log = 0;  // TerrainLog: terrain statistics, hole checks and debug keys (fixes_terrain.cpp)
  int far_tree_log = 0;  // FarTreeLog: statistics every 5 s and a check against the game's billboards; 2 also tints far trees
  bool draw_batch = true;     // DrawBatch: send 3D draws in a row with the same state as one draw (device.cpp)
  bool batch_log = false;     // BatchLog: log draw batching every 5 s
  bool draw_log = false;      // DrawLog: measure which draw calls could be merged (dd/drawlog.cpp)
  bool dd_stats = false;      // DDStats: log renderer activity every 2 seconds
  int renderer = 11;          // Renderer: 11 = Direct3D 11, 9 = Direct3D 9 (also used when D3D11 is missing)
  bool exclusive = false;     // Fullscreen: false = borderless window (default), true = exclusive fullscreen
  bool card_test = false;     // CardTest: 1 = run the game's own video card test (0 = MCM2MaX's fixed values)
  int monitor = 1;            // Monitor: 1 = primary display, 2.. = the other displays
  int bl_width = 0, bl_height = 0;             // BorderlessResolution: 0 = the desktop's
  int fs_width = 0, fs_height = 0, fs_hz = 0;  // ExclusiveResolution: 0 = the desktop's; hz 0 = highest
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
  int frame_cap = 500;        // FrameCap: most frames per second (0 = no cap beyond VSync)
  int menu_fps_cap = 240;     // MenuFpsCap: most frames per second in the menus (0 = no menu cap)
  bool precise_timer = true;  // PreciseTimer: frame time from QueryPerformanceCounter, not whole ms
  bool frame_fix = true;      // FrameFix: per-frame animation/physics steps scale with frame time
  int ai_rate = 30;           // AiRate: computer riders re-plan this many times a second (0 = every frame)
  int smooth_log = 0;         // SmoothLog: 1 = trace a few frames of matrices and draws, 2 = camera only, 120 frames, 3 = rider shadows
  int reflections = 2;     // Reflections (also read by dd/reflect.cpp through here)
  int rider_shadows = 2;      // RiderShadows: 0 = the game's CPU shadow, 1 = GPU shadow for the rider the camera follows, 2 = GPU shadows for every rider (dd/gpushadow.cpp)
  int shadow_strength = 0;    // ShadowStrength: GPU shadow darkness in percent (0 = the game's own)
  int shadow_softness = 100;  // ShadowSoftness: how much GPU shadows blur and fade with the caster's height above the ground, percent (0 = sharp)
  int shadow_log = 0;         // ShadowLog: 1 = GPU shadow counters every 5 s, 2 = also trace every shadow window for 90 frames
  int sim_smooth = 1;         // SimSmooth: draw objects between simulation steps (needs SimRate)
  bool fast_sort = true;      // FastSort: replace the CRT qsort (quadratic on equal keys) with a merge sort (fixes_sort.cpp)
  int tree_cache_mb = 32;     // TreeCacheMB: trim the vegetation cache only when this much is unused (0 = original 256 KB)
  bool sort_log = false;      // SortLog: sort calls per caller every 10 s
  bool input_timing = true;   // InputTiming: race steps get the input of their own time slice; taps held one step (input_timing.cpp)
  bool alt_tab_fix = true;    // AltTabFix: keys let go while alt-tabbed are released; lost devices acquired again (input_timing.cpp)
  bool input_log = false;     // InputLog: log input timing (events per step, held taps, joystick samples)
  bool xinput = true;         // XInput: Xbox controllers as analog DirectInput joysticks with rumble (xinput_pad.cpp)
  int xinput_rumble = 100;    // XInputRumble: rumble strength in percent (0 = off, no force feedback offered)
  int xinput_deadzone = 15;   // XInputDeadzone: stick dead zone in percent of the stick's travel
  int xinput_steer_curve = 33;    // XInputSteerCurve: left stick X exponent in percent (game cubes it on top)
  int xinput_steer_smooth = 60;    // XInputSteerSmooth: ms the steering takes to ease toward the stick (0 = off)
  bool controller_hub = true;     // ControllerHub: all gamepads (SDL3) as one game controller, last touched wins (pad_hub.cpp)
  bool controller_picker = false; // ControllerPicker: show the game's startup "choose a controller" box
  char disabled_controllers[512] = {};  // DisabledControllers: VID:PID or name parts, comma separated, the hub ignores
  bool keyboard_pad = true;       // KeyboardPad: driving keys drive the controller hub too
  char keyboard_keys[8][24] = {};  // KeySteerLeft..KeyStunt2 (pad_hub.cpp)
  bool keep_control_preset = true;  // KeepControlPreset: keep the chosen control preset between runs
  int xinput_rumble_min_ms = 150;   // XInputRumbleMinMs: shortest rumble, held at full strength before fading
  int xinput_rumble_threshold = 10;  // XInputRumbleThreshold: % jump in the game's stick force that rumbles
  int xinput_rumble_threshold_event[6] = {};  // QuarryRumbleThreshold..EnduroRumbleThreshold (event number order), 0 = XInputRumbleThreshold
  bool xinput_rumble_forces = false;  // XInputRumbleForces: the game's steady stick forces rumble too
  bool xinput_trigger_buttons = true;   // XInputTriggerButtons: triggers also as buttons 11 (LT) and 12 (RT)
  bool xinput_log = false;    // XInputLog: log controller calls, buttons, effects and a summary every 10 s
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
  int motion_blur = 0;        // MotionBlur: speed-based blur of the 3D scene (not the HUD) at speed (dd/motionblur.cpp)
  int motion_blur_strength = 80;   // MotionBlurStrength: blur length in percent (100 = 8% of the way to the focus; 0..300)
  int motion_blur_min_speed = 30;  // MotionBlurMinSpeed: mph where the blur starts
  int motion_blur_max_speed = 70;  // MotionBlurMaxSpeed: mph where it is full
  int motion_blur_center = 60;     // MotionBlurCenter: size of the sharp middle, percent of the way to the screen edges
  bool motion_blur_log = false;    // MotionBlurLog: speed, focus point, where the blur went in the frame, every 5 s
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
  bool texture_decode_thread = true;  // TextureDecodeThread: decode replacement pictures off the game thread
  bool texture_preload = true;        // TexturePreload: decode every replacement picture at startup
  int texture_preload_mb = 256;       // TexturePreloadMB: most decoded picture memory preloaded
  bool true_color_textures = true;  // TrueColorTextures: load textures as 24/32-bit instead of 16-bit
  bool texture_log = false;         // TextureLog: log the format of each texture the game creates
  bool menu_options = true;   // MenuOptions: MCM2MaX settings added to the game's own Options pages (menus.cpp)
  bool menu_log = false;      // MenuLog: log every event on the pages MCM2MaX adds settings to
  bool menu_max = true;       // MenuMaX: new menu screens drawn with RmlUi from UI/MaX/ (ui/ui.cpp)
  bool max_menus = true;      // MaXMenus: the game's menus drawn by MenuMaX (UI/MaX/menus/*.rml) over the game's own
  bool zone_fix = true;       // ZoneFix: Multiplayer's MSN Gaming Zone button greyed out and does nothing (it crashed / quit the game)
  bool menu_nav = true;       // MenuNav: keyboard / controllers drive the game's own menus (menunav.cpp)
  bool menu_hires_3d = true;  // MenuHiRes3D: 3D in the menus (garage bike) drawn at window resolution (dd/hires3d.cpp)
  bool menu_flag = true;      // MenuFlag: waving flag behind the main menu, under MenuBackground (dd/menubg.cpp)
  int menu_flag_speed = 100;       // MenuFlagSpeed: % of the default movement speed
  int menu_flag_brightness = 68;   // MenuFlagBrightness: % (100 = preview-5 colours)
  int menu_flag_warp = 100;        // MenuFlagWarp: % of how far the chequers are pushed around
  int menu_flag_shading = 100;     // MenuFlagShading: % of the light, shadow and mottling
  char menu_background[260] = {};  // MenuBackground: 640x480 picture with transparency, replaces the main menu background
};

const Config& GetConfig();
void LoadConfig();

// Settings file names. Until 2026-11 the project was called mcm2fix; a game
// folder with only the old mcm2fix.ini (section [mcm2fix]) still works.
constexpr char kIniFile[] = "mcm2max.ini";
constexpr char kIniSection[] = "MCM2MaX";
constexpr char kMenuIniFile[] = "MCM2MaX-menu.ini";
constexpr char kLegacyIniFile[] = "mcm2fix.ini";
constexpr char kLegacyIniSection[] = "mcm2fix";
constexpr char kLegacyMenuIniFile[] = "mcm2fix-menu.ini";

// The settings file in use: mcm2max.ini next to Mcm2.exe, or mcm2fix.ini if
// only that exists. Its section: [MCM2MaX], or [mcm2fix] in an old file.
const char* IniPath();
const char* IniSection();

// Settings changed from the game's menus (MenuOptions) are saved in
// %LOCALAPPDATA%\Motocross Madness 2\MCM2MaX-menu.ini and win over mcm2max.ini.
// Empty if there is no per-user folder.
const char* MenuIniPath();
// Save a setting chosen in the menus: to mcm2max.ini when it can be written,
// else to MenuIniPath(); returns the file used. MotionBlur, VSync and
// AlphaToCoverage and Anisotropy also change at once; the rest apply at the next start.
const char* SetFromMenu(const char* key, int value);
// The same for a text value (e.g. Resolution=1920x1080).
const char* SetStringFromMenu(const char* key, const char* value);
// Options > Graphics Borderless / Exclusive: changes Fullscreen now and saves it.
const char* SetFullscreenFromMenu(bool exclusive);
// Moves old Resolution= / FullscreenMode= values to BorderlessResolution= /
// ExclusiveResolution= (once; logs).
void MigrateDisplayKeys();
// Resolution (fixes_resolution.cpp): the mode the game renders races at, and
// with Fullscreen=Exclusive the screen mode: the player's pick or the ini's
// BorderlessResolution / ExclusiveResolution, else the desktop's. hz 0 = any.
void ResolutionWanted(int* w, int* h, int* hz);
// Options > Graphics Borderless / Exclusive: switches now, refills the game's
// mode list and the resolution dropdown (its control, or 0).
void ResolutionSetExclusive(bool exclusive, uintptr_t dropdown);
// Options > Graphics, every frame: a new pick in the resolution dropdown
// (its control, or 0) is applied and saved now.
void ResolutionPickNow(uintptr_t dropdown);
// Options > Graphics set-up: the resolution dropdown with "Desktop" first.
void ResolutionFillDropdown(uintptr_t dropdown);
// A value read from mcm2max.ini, replaced by the menus' choice if there is one.
int MenuOverride(const char* key, int value);
// A setting's text as saved: the menus' file first, then mcm2max.ini (any
// trailing ; comment cut). False if neither has the key.
bool ReadSetting(const char* key, std::string* out);
