#include "config.h"
#include "patch.h"
#include "xp_compat.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
Config g_config;

// Paths and section names of the two ini files (see IniPath / MenuIniPath).
char g_ini_path[MAX_PATH] = {};
char g_ini_section[16] = {};
char g_menu_section[16] = {};

bool Exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

// The section a settings file uses: [MCM2MaX], or [mcm2fix] in a file from
// before the rename that has only that one.
void SectionOf(const char* path, char* out, size_t size) {
  std::snprintf(out, size, "%s", kIniSection);
  char names[4096] = {};
  const DWORD n = GetPrivateProfileSectionNamesA(names, sizeof(names), path);
  bool legacy = false;
  for (const char* p = names; p < names + n && *p; p += std::strlen(p) + 1) {
    if (!_stricmp(p, kIniSection)) return;
    if (!_stricmp(p, kLegacyIniSection)) legacy = true;
  }
  if (legacy) std::snprintf(out, size, "%s", kLegacyIniSection);
}

void FindIni() {
  if (g_ini_path[0]) return;
  char dir[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, dir, MAX_PATH);
  if (char* slash = std::strrchr(dir, '\\')) slash[1] = '\0';
  char legacy[MAX_PATH] = {};
  xp::Copy(g_ini_path, dir);
  xp::Append(g_ini_path, kIniFile);
  xp::Copy(legacy, dir);
  xp::Append(legacy, kLegacyIniFile);
  // An install from before the rename keeps working until the file is renamed.
  if (!Exists(g_ini_path) && Exists(legacy)) xp::Copy(g_ini_path, legacy);
  SectionOf(g_ini_path, g_ini_section, sizeof(g_ini_section));
}

float ReadFloat(const char* key, float def, const char* ini) {
  char buf[64] = {};
  GetPrivateProfileStringA(g_ini_section, key, "", buf, sizeof(buf), ini);
  if (!buf[0]) return def;
  char* end = nullptr;
  double v = std::strtod(buf, &end);
  return end != buf ? static_cast<float>(v) : def;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

char g_menu_ini[MAX_PATH] = {};
// Values of the old Resolution= / FullscreenMode= keys still to be moved to
// BorderlessResolution= / ExclusiveResolution= (MigrateDisplayKeys).
char g_display_migrate[2][48] = {};
char g_game_ini[MAX_PATH] = {};  // mcm2max.ini next to Mcm2.exe

// A key set from the menus (MCM2MaX-menu.ini) wins over mcm2max.ini.
int ReadMenuInt(const char* key, int value) {
  if (!g_config.menu_options || !g_menu_ini[0]) return value;
  return GetPrivateProfileIntA(g_menu_section, key, value, g_menu_ini);
}
}

const char* IniPath() {
  FindIni();
  return g_ini_path;
}

const char* IniSection() {
  FindIni();
  return g_ini_section;
}

const char* MenuIniPath() { return g_menu_ini; }

int MenuOverride(const char* key, int value) { return ReadMenuInt(key, value); }

bool ReadSetting(const char* key, std::string* out) {
  char buf[512] = {};
  if (g_menu_ini[0]) {
    GetPrivateProfileStringA(g_menu_section, key, "\x01", buf, sizeof(buf), g_menu_ini);
    if (std::strcmp(buf, "\x01") != 0) {
      *out = buf;
      return true;
    }
  }
  if (g_game_ini[0]) {
    GetPrivateProfileStringA(g_ini_section, key, "\x01", buf, sizeof(buf), g_game_ini);
    if (std::strcmp(buf, "\x01") != 0) {
      std::string v = buf;
      // Values may carry a trailing ; comment.
      const size_t c = v.find(';');
      if (c != std::string::npos) v.erase(c);
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
      *out = v;
      return true;
    }
  }
  return false;
}

const char* SetFromMenu(const char* key, int value) {
  if (!std::strcmp(key, "MotionBlur")) g_config.motion_blur = value != 0;
  else if (!std::strcmp(key, "VSync")) g_config.vsync = value != 0;
  else if (!std::strcmp(key, "AlphaToCoverage")) g_config.alpha_to_coverage = value;
  else if (!std::strcmp(key, "Anisotropy")) g_config.anisotropy = value;
  char v[16];
  std::snprintf(v, sizeof(v), "%d", value);
  if (!std::strcmp(key, "ViewDistance")) std::snprintf(v, sizeof(v), "%g", value / 100.0);  // value in hundredths
  return SetStringFromMenu(key, v);
}

const char* SetFullscreenFromMenu(bool exclusive) {
  g_config.exclusive = exclusive;
  return SetStringFromMenu("Fullscreen", exclusive ? "Exclusive" : "Borderless");
}

const char* SetStringFromMenu(const char* key, const char* v) {
  // mcm2max.ini itself when it can be written (only that key's line changes);
  // then any older choice for the key in the per-user file is dropped so it
  // can't win at the next start. Otherwise the per-user file.
  const DWORD attr = g_game_ini[0] ? GetFileAttributesA(g_game_ini) : INVALID_FILE_ATTRIBUTES;
  if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_READONLY) &&
      WritePrivateProfileStringA(g_ini_section, key, v, g_game_ini)) {
    if (g_menu_ini[0] && GetFileAttributesA(g_menu_ini) != INVALID_FILE_ATTRIBUTES)
      WritePrivateProfileStringA(g_menu_section, key, nullptr, g_menu_ini);
    return g_game_ini;
  }
  if (!g_menu_ini[0]) return "(nowhere: no per-user folder)";
  CreateDirectoryA(xp::LocalAppData().append("\\Motocross Madness 2").c_str(), nullptr);
  WritePrivateProfileStringA(g_menu_section, key, v, g_menu_ini);
  return g_menu_ini;
}

void MigrateDisplayKeys() {
  static const char* const kNew[2] = {"BorderlessResolution", "ExclusiveResolution"};
  static const char* const kOld[2] = {"Resolution", "FullscreenMode"};
  for (int i = 0; i < 2; ++i) {
    if (!g_display_migrate[i][0]) continue;
    char v[48];
    std::snprintf(v, sizeof(v), "%s", g_display_migrate[i]);
    v[std::strcspn(v, " \t;")] = 0;
    int w = 0, h = 0, hz = 0;
    const int n = std::sscanf(v, "%dx%d@%d", &w, &h, &hz);
    if (n < 2) std::snprintf(v, sizeof(v), "Desktop");  // Desktop, Resolution, Game: all mean the desktop now
    if (i == 0 && n >= 2) std::snprintf(v, sizeof(v), "%dx%d", w, h);
    const char* file = SetStringFromMenu(kNew[i], v);
    const DWORD attr = GetFileAttributesA(g_game_ini);
    const bool removed = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_READONLY) &&
                         WritePrivateProfileStringA(g_ini_section, kOld[i], nullptr, g_game_ini);
    patch::Log("config: old %s=%s moved to %s=%s in %s (old key %s)", kOld[i], g_display_migrate[i], kNew[i], v, file,
               removed ? "removed" : "left in place");
    g_display_migrate[i][0] = 0;
  }
}

const Config& GetConfig() { return g_config; }

void LoadConfig() {
  FindIni();
  const char* ini = g_ini_path;
  const char* sec = g_ini_section;
  xp::Copy(g_game_ini, ini);
  // First: the menus' saved choices (ReadMenuInt) need these.
  g_config.menu_options = GetPrivateProfileIntA(sec, "MenuOptions", 1, ini) != 0;
  {
    const std::string dir = xp::LocalAppData();
    if (!dir.empty()) {
      const std::string base = dir + "\\Motocross Madness 2\\";
      xp::Copy(g_menu_ini, (base + kMenuIniFile).c_str());
      // Menu choices saved before the rename, until something new is saved.
      const std::string legacy = base + kLegacyMenuIniFile;
      if (!Exists(g_menu_ini) && Exists(legacy.c_str())) xp::Copy(g_menu_ini, legacy.c_str());
      SectionOf(g_menu_ini, g_menu_section, sizeof(g_menu_section));
    }
  }
  g_config.one_core = GetPrivateProfileIntA(sec, "OneCore", 0, ini) != 0;
  g_config.log = GetPrivateProfileIntA(sec, "Log", 1, ini) != 0;
  g_config.startup = GetPrivateProfileIntA(sec, "Startup", 1, ini) != 0;
  g_config.registry = GetPrivateProfileIntA(sec, "Registry", 1, ini) != 0;
  g_config.files = GetPrivateProfileIntA(sec, "Files", 1, ini) != 0;
  g_config.reg_log = GetPrivateProfileIntA(sec, "RegLog", 0, ini) != 0;
  g_config.wide_fov = ReadMenuInt("WideFov", GetPrivateProfileIntA(sec, "WideFov", 1, ini)) != 0;
  g_config.alpha_cut = GetPrivateProfileIntA(sec, "AlphaCut", 128, ini);
  g_config.menu_log = GetPrivateProfileIntA(sec, "MenuLog", 0, ini) != 0;
  g_config.menu_flag = GetPrivateProfileIntA(sec, "MenuFlag", 1, ini) != 0;
  g_config.menu_hires_3d = GetPrivateProfileIntA(sec, "MenuHiRes3D", 1, ini) != 0;
  g_config.menu_max = GetPrivateProfileIntA(sec, "MenuMaX", 1, ini) != 0;
  // (MaXOptions: its name in the first test builds.)
  g_config.max_menus = GetPrivateProfileIntA(sec, "MaXMenus", GetPrivateProfileIntA(sec, "MaXOptions", 1, ini), ini) != 0;
  g_config.menu_nav = GetPrivateProfileIntA(sec, "MenuNav", 1, ini) != 0;
  g_config.zone_fix = GetPrivateProfileIntA(sec, "ZoneFix", 1, ini) != 0;
  {
    auto pct = [&](const char* key, int def, int hi) {
      const int v = static_cast<int>(GetPrivateProfileIntA(sec, key, def, ini));
      return v < 0 ? 0 : v > hi ? hi : v;
    };
    g_config.menu_flag_speed = pct("MenuFlagSpeed", 100, 1000);
    g_config.menu_flag_brightness = pct("MenuFlagBrightness", 68, 300);
    g_config.menu_flag_warp = pct("MenuFlagWarp", 100, 500);
    g_config.menu_flag_shading = pct("MenuFlagShading", 100, 300);
  }
  {
    // A path: stop at a ';' comment, trim the ends.
    char v[260] = {};
    GetPrivateProfileStringA(sec, "MenuBackground", "UI\\MAINMENU\\01_MAIN\\BackGround_01.png", v, sizeof(v), ini);
    if (char* semi = std::strchr(v, ';')) *semi = '\0';
    size_t n = std::strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = '\0';
    const char* start = v;
    while (*start == ' ' || *start == '\t') ++start;
    xp::Copy(g_config.menu_background, *start ? start : "UI\\MAINMENU\\01_MAIN\\BackGround_01.png");
  }
  g_config.motion_blur = ReadMenuInt("MotionBlur", GetPrivateProfileIntA(sec, "MotionBlur", 1, ini)) != 0;
  g_config.motion_blur_strength = Clamp(GetPrivateProfileIntA(sec, "MotionBlurStrength", 80, ini), 0, 300);
  g_config.motion_blur_min_speed = Clamp(GetPrivateProfileIntA(sec, "MotionBlurMinSpeed", 30, ini), 0, 200);
  g_config.motion_blur_max_speed = Clamp(GetPrivateProfileIntA(sec, "MotionBlurMaxSpeed", 70, ini), 1, 250);
  if (g_config.motion_blur_max_speed <= g_config.motion_blur_min_speed)
    g_config.motion_blur_max_speed = g_config.motion_blur_min_speed + 1;
  g_config.motion_blur_center = Clamp(GetPrivateProfileIntA(sec, "MotionBlurCenter", 60, ini), 0, 95);
  g_config.motion_blur_log = GetPrivateProfileIntA(sec, "MotionBlurLog", 0, ini) != 0;
  g_config.alpha_to_coverage = ReadMenuInt("AlphaToCoverage", GetPrivateProfileIntA(sec, "AlphaToCoverage", 2, ini));
  if (g_config.alpha_to_coverage < 0 || g_config.alpha_to_coverage > 2) g_config.alpha_to_coverage = 2;
  g_config.alpha_log = GetPrivateProfileIntA(sec, "AlphaLog", 0, ini) != 0;
  g_config.vsync = ReadMenuInt("VSync", GetPrivateProfileIntA(sec, "VSync", 1, ini)) != 0;
  g_config.frame_cap = GetPrivateProfileIntA(sec, "FrameCap", 500, ini);
  if (g_config.frame_cap < 0) g_config.frame_cap = 0;
  g_config.menu_fps_cap = GetPrivateProfileIntA(sec, "MenuFpsCap", 240, ini);
  if (g_config.menu_fps_cap < 0) g_config.menu_fps_cap = 0;
  g_config.precise_timer = GetPrivateProfileIntA(sec, "PreciseTimer", 1, ini) != 0;
  g_config.frame_fix = GetPrivateProfileIntA(sec, "FrameFix", 1, ini) != 0;
  g_config.ref_rate = GetPrivateProfileIntA(sec, "RefRate", 30, ini);
  if (g_config.ref_rate < 10 || g_config.ref_rate > 240) g_config.ref_rate = 30;
  g_config.ai_rate = GetPrivateProfileIntA(sec, "AiRate", 30, ini);
  if (g_config.ai_rate < 0) g_config.ai_rate = 0;
  {
    static const char* kLevel[3] = {"Easy", "Normal", "Hard"};
    static const int kAccel[3] = {80, 130, 150};
    static const int kTop[3] = {90, 100, 100};
    for (int d = 0; d < 3; ++d) {
      char key[32];
      std::snprintf(key, sizeof(key), "AiAccel%s", kLevel[d]);
      int v = GetPrivateProfileIntA(sec, key, kAccel[d], ini);
      g_config.ai_accel[d] = v < 10 ? 10 : v > 400 ? 400 : v;
      std::snprintf(key, sizeof(key), "AiTopSpeed%s", kLevel[d]);
      v = GetPrivateProfileIntA(sec, key, kTop[d], ini);
      g_config.ai_top_speed[d] = v < 50 ? 50 : v > 150 ? 150 : v;
    }
    g_config.ai_log = GetPrivateProfileIntA(sec, "AiLog", 0, ini) != 0;
  }
  g_config.ai_pick_fix = GetPrivateProfileIntA(sec, "AiPickFix", 1, ini) != 0;
  g_config.ai_steer_rate = GetPrivateProfileIntA(sec, "AiSteerRate", 0, ini);
  if (g_config.ai_steer_rate < 0) g_config.ai_steer_rate = 0;
  g_config.smooth_log = GetPrivateProfileIntA(sec, "SmoothLog", 0, ini);
  g_config.rider_shadows = ReadMenuInt("RiderShadows", GetPrivateProfileIntA(sec, "RiderShadows", 2, ini));
  g_config.reflections = ReadMenuInt("Reflections", GetPrivateProfileIntA(sec, "Reflections", 2, ini));
  if (g_config.reflections < 0 || g_config.reflections > 2) g_config.reflections = 2;
  if (g_config.rider_shadows < 0 || g_config.rider_shadows > 2) g_config.rider_shadows = 2;
  g_config.shadow_strength = GetPrivateProfileIntA(sec, "ShadowStrength", 0, ini);
  if (g_config.shadow_strength < 0 || g_config.shadow_strength > 100) g_config.shadow_strength = 0;
  g_config.shadow_softness = GetPrivateProfileIntA(sec, "ShadowSoftness", 100, ini);
  if (g_config.shadow_softness < 0 || g_config.shadow_softness > 400) g_config.shadow_softness = 100;
  g_config.shadow_log = GetPrivateProfileIntA(sec, "ShadowLog", 0, ini);
  g_config.sim_smooth = GetPrivateProfileIntA(sec, "SimSmooth", 1, ini) != 0;
  g_config.sim_rate = GetPrivateProfileIntA(sec, "SimRate", 30, ini);
  if (g_config.sim_rate < 0) g_config.sim_rate = 0;
  if (g_config.sim_rate > 0 && g_config.sim_rate < 10) g_config.sim_rate = 10;
  if (g_config.sim_rate > 1000) g_config.sim_rate = 1000;
  g_config.arrow_heading = GetPrivateProfileIntA(sec, "ArrowFix", 1, ini) != 0;
  char buf[32] = {};
  GetPrivateProfileStringA(sec, "ArrowScale", "2.0", buf, sizeof(buf), ini);
  float scale = static_cast<float>(std::atof(buf));
  g_config.arrow_scale = (scale >= 0.25f && scale <= 8.0f) ? scale : 2.0f;
  GetPrivateProfileStringA(sec, "ArrowTurn", "0.8", buf, sizeof(buf), ini);
  float turn = static_cast<float>(std::atof(buf));
  g_config.arrow_turn = (turn >= 0.0f && turn <= 2.0f) ? turn : 0.8f;
  GetPrivateProfileStringA(sec, "ArrowTilt", "10", buf, sizeof(buf), ini);
  float tilt = static_cast<float>(std::atof(buf));
  g_config.arrow_log = GetPrivateProfileIntA(sec, "ArrowLog", 0, ini) != 0;
  g_config.fov_log = GetPrivateProfileIntA(sec, "FovLog", 0, ini) != 0;
  g_config.arrow_tilt = (tilt >= -45.0f && tilt <= 45.0f) ? tilt : 10.0f;
  g_config.restart_fix = GetPrivateProfileIntA(sec, "RestartFix", 1, ini) != 0;
  g_config.restart_log = GetPrivateProfileIntA(sec, "RestartLog", 0, ini) != 0;
  g_config.profile = GetPrivateProfileIntA(sec, "Profile", 0, ini) != 0;
  g_config.profile_sample_us = GetPrivateProfileIntA(sec, "ProfileSampleUs", 1000, ini);
  g_config.fast_crt = GetPrivateProfileIntA(sec, "FastCrt", 1, ini) != 0;
  g_config.fast_ftol = GetPrivateProfileIntA(sec, "FastFtol", 1, ini) != 0;
  g_config.simd = GetPrivateProfileIntA(sec, "Simd", 1, ini) != 0;
  g_config.simd_log = GetPrivateProfileIntA(sec, "SimdLog", 0, ini) != 0;
  g_config.pause_on_focus_loss = GetPrivateProfileIntA(sec, "PauseOnFocusLoss", 1, ini) != 0;
  g_config.dpi_aware = GetPrivateProfileIntA(sec, "DpiAware", 1, ini) != 0;
  g_config.race_power = GetPrivateProfileIntA(sec, "RacePower", 1, ini);
  g_config.race_power_async = GetPrivateProfileIntA(sec, "RacePowerAsync", 1, ini);
  g_config.fast_game_code = GetPrivateProfileIntA(sec, "FastGameCode", 1, ini) != 0;
  g_config.async_sound = GetPrivateProfileIntA(sec, "AsyncSound", 2, ini);
  g_config.sound_max_distance_mute = GetPrivateProfileIntA(sec, "SoundMaxDistanceMute", 1, ini);
  g_config.fast_sort = GetPrivateProfileIntA(sec, "FastSort", 1, ini) != 0;
  g_config.tree_cache_mb = GetPrivateProfileIntA(sec, "TreeCacheMB", 32, ini);
  if (g_config.tree_cache_mb < 0) g_config.tree_cache_mb = 0;
  if (g_config.tree_cache_mb > 512) g_config.tree_cache_mb = 512;
  g_config.sort_log = GetPrivateProfileIntA(sec, "SortLog", 0, ini) != 0;
  g_config.input_timing = GetPrivateProfileIntA(sec, "InputTiming", 1, ini) != 0;
  g_config.alt_tab_fix = GetPrivateProfileIntA(sec, "AltTabFix", 1, ini) != 0;
  g_config.input_log = GetPrivateProfileIntA(sec, "InputLog", 0, ini) != 0;
  g_config.xinput = GetPrivateProfileIntA(sec, "XInput", 1, ini) != 0;
  g_config.xinput_rumble = GetPrivateProfileIntA(sec, "XInputRumble", 100, ini);
  if (g_config.xinput_rumble < 0) g_config.xinput_rumble = 0;
  if (g_config.xinput_rumble > 200) g_config.xinput_rumble = 200;
  g_config.xinput_deadzone = GetPrivateProfileIntA(sec, "XInputDeadzone", 15, ini);
  if (g_config.xinput_deadzone < 0) g_config.xinput_deadzone = 0;
  if (g_config.xinput_deadzone > 50) g_config.xinput_deadzone = 50;
  g_config.xinput_steer_curve = GetPrivateProfileIntA(sec, "XInputSteerCurve", 33, ini);
  if (g_config.xinput_steer_curve < 33) g_config.xinput_steer_curve = 33;
  if (g_config.xinput_steer_curve > 300) g_config.xinput_steer_curve = 300;
  g_config.xinput_steer_smooth = GetPrivateProfileIntA(sec, "XInputSteerSmooth", 60, ini);
  if (g_config.xinput_steer_smooth < 0) g_config.xinput_steer_smooth = 0;
  if (g_config.xinput_steer_smooth > 500) g_config.xinput_steer_smooth = 500;
  g_config.controller_hub = GetPrivateProfileIntA(sec, "ControllerHub", 1, ini) != 0;
  g_config.controller_picker = GetPrivateProfileIntA(sec, "ControllerPicker", 0, ini) != 0;
  GetPrivateProfileStringA(sec, "DisabledControllers", "", g_config.disabled_controllers,
                           sizeof(g_config.disabled_controllers), ini);
  g_config.keyboard_pad = GetPrivateProfileIntA(sec, "KeyboardPad", 1, ini) != 0;
  {
    // The game's own default keyboard driving keys (MCM2 help: Default
    // Keyboard Driving Controls; Z/X for stunts).
    const char* keys[8] = {"KeySteerLeft", "KeySteerRight", "KeyLeanForward", "KeyLeanBack",
                           "KeyThrottle", "KeyBrake", "KeyStunt1", "KeyStunt2"};
    const char* defs[8] = {"Left", "Right", "W", "S", "Up", "Down", "Z", "X"};
    for (int k = 0; k < 8; ++k) {
      char* v = g_config.keyboard_keys[k];
      GetPrivateProfileStringA(sec, keys[k], defs[k], v, sizeof(g_config.keyboard_keys[k]), ini);
      v[std::strcspn(v, "\t;")] = 0;  // drop an inline comment
      size_t n = std::strlen(v);
      while (n && v[n - 1] == ' ') v[--n] = 0;
    }
  }
  g_config.keep_control_preset = GetPrivateProfileIntA(sec, "KeepControlPreset", 1, ini) != 0;
  g_config.xinput_rumble_min_ms = GetPrivateProfileIntA(sec, "XInputRumbleMinMs", 150, ini);
  if (g_config.xinput_rumble_min_ms < 0) g_config.xinput_rumble_min_ms = 0;
  if (g_config.xinput_rumble_min_ms > 1000) g_config.xinput_rumble_min_ms = 1000;
  g_config.xinput_rumble_threshold = GetPrivateProfileIntA(sec, "XInputRumbleThreshold", 10, ini);
  if (g_config.xinput_rumble_threshold < 1) g_config.xinput_rumble_threshold = 1;
  if (g_config.xinput_rumble_threshold > 100) g_config.xinput_rumble_threshold = 100;
  {
    // Event numbers as in game+0x2d78: 0 Quarry, 1 Baja, 2 Nationals, 3 Supercross, 4 Moto-Tag, 5 Enduro.
    const char* keys[6] = {"QuarryRumbleThreshold", "BajaRumbleThreshold", "NationalsRumbleThreshold",
                           "SupercrossRumbleThreshold", "TagRumbleThreshold", "EnduroRumbleThreshold"};
    const int defs[6] = {0, 0, 20, 20, 0, 0};
    for (int e = 0; e < 6; ++e) {
      int v = GetPrivateProfileIntA(sec, keys[e], defs[e], ini);
      g_config.xinput_rumble_threshold_event[e] = v < 0 ? 0 : v > 100 ? 100 : v;
    }
  }
  g_config.xinput_rumble_forces = GetPrivateProfileIntA(sec, "XInputRumbleForces", 0, ini) != 0;
  g_config.xinput_trigger_buttons = GetPrivateProfileIntA(sec, "XInputTriggerButtons", 1, ini) != 0;
  g_config.xinput_log = GetPrivateProfileIntA(sec, "XInputLog", 0, ini) != 0;
  g_config.sound_log = GetPrivateProfileIntA(sec, "SoundLog", 0, ini) != 0;
  {
    // Event numbers as in game+0x2d74/0x2d78: 0 Quarry, 1 Baja, 2 Nationals, 3 Supercross, 4 Moto-Tag, 5 Enduro.
    const char* keys[6] = {"QuarryReverb", "BajaReverb", "NationalsReverb", "SupercrossReverb", "TagReverb", "EnduroReverb"};
    const char* defs[6] = {"Mountains", "Mountains", "Forest", "Default", "Default", "Forest"};
    for (int e = 0; e < 6; ++e) {
      char* v = g_config.outdoor_reverb[e];
      if (!keys[e]) continue;
      GetPrivateProfileStringA(sec, keys[e], defs[e], v, sizeof(g_config.outdoor_reverb[e]), ini);
      v[std::strcspn(v, "\t; ")] = 0;  // drop an inline comment
    }
  }
  g_config.background_mute = GetPrivateProfileIntA(sec, "BackgroundMute", 1, ini) != 0;
  g_config.sound_pause_log = GetPrivateProfileIntA(sec, "SoundPauseLog", 0, ini) != 0;
  g_config.state_cache = GetPrivateProfileIntA(sec, "StateCache", 1, ini) != 0;
  g_config.dd_stats = GetPrivateProfileIntA(sec, "DDStats", 0, ini) != 0;
  g_config.draw_batch = GetPrivateProfileIntA(sec, "DrawBatch", 1, ini) != 0;
  g_config.still_scenery = GetPrivateProfileIntA(sec, "StillScenery", 1, ini) != 0;
  g_config.fast_object_walk = GetPrivateProfileIntA(sec, "FastObjectWalk", 1, ini) != 0;
  g_config.walk_log = GetPrivateProfileIntA(sec, "WalkLog", 0, ini) != 0;
  g_config.far_trees = GetPrivateProfileIntA(sec, "FarTrees", 1, ini) != 0;
  g_config.far_tree_cells = GetPrivateProfileIntA(sec, "FarTreeCells", 1, ini) != 0;
  g_config.far_tree_distance = static_cast<float>(GetPrivateProfileIntA(sec, "FarTreeDistance", 0, ini));
  if (g_config.far_tree_distance < 0) g_config.far_tree_distance = 0;
  g_config.far_tree_log = GetPrivateProfileIntA(sec, "FarTreeLog", 0, ini);
  g_config.terrain_lod_fix = GetPrivateProfileIntA(sec, "TerrainLodFix", 1, ini) != 0;
  g_config.terrain_hole_fix = GetPrivateProfileIntA(sec, "TerrainHoleFix", 1, ini) != 0;
  g_config.terrain_log = GetPrivateProfileIntA(sec, "TerrainLog", 0, ini);
  g_config.fast_terrain = GetPrivateProfileIntA(sec, "FastTerrain", 1, ini) != 0;
  g_config.terrain_check = GetPrivateProfileIntA(sec, "TerrainCheck", 0, ini);
  g_config.terrain_mesh_cache_kb = GetPrivateProfileIntA(sec, "TerrainMeshCache", 32768, ini);
  g_config.gpu_terrain = GetPrivateProfileIntA(sec, "GpuTerrain", 1, ini) != 0;
  g_config.gpu_terrain_detail = GetPrivateProfileIntA(sec, "GpuTerrainDetail", 1, ini) != 0;
  g_config.gpu_terrain_detail_reach = static_cast<float>(GetPrivateProfileIntA(sec, "GpuTerrainDetailReach", 0, ini));
  g_config.gpu_terrain_far = GetPrivateProfileIntA(sec, "GpuTerrainFar", 1, ini);
  g_config.gpu_terrain_log = GetPrivateProfileIntA(sec, "GpuTerrainLog", 1, ini);
  g_config.free_cam = GetPrivateProfileIntA(sec, "FreeCam", 1, ini);
  g_config.free_cam_speed = static_cast<float>(GetPrivateProfileIntA(sec, "FreeCamSpeed", 60, ini));
  g_config.free_cam_mouse = GetPrivateProfileIntA(sec, "FreeCamMouse", 15, ini);
  g_config.batch_log = GetPrivateProfileIntA(sec, "BatchLog", 0, ini) != 0;
  g_config.draw_log = GetPrivateProfileIntA(sec, "DrawLog", 0, ini) != 0;
  {
    char r[32] = {};
    GetPrivateProfileStringA(sec, "Renderer", "D3D11", r, sizeof(r), ini);
    // Windows keeps an inline "; comment" in the value, so only the first
    // word counts. Accepts D3D9 / DX9 / 9 and D3D11 / DX11 / 11.
    r[std::strcspn(r, " \t;")] = 0;
    g_config.renderer = (std::strstr(r, "9") && !std::strstr(r, "11")) ? 9 : 11;
  }
  {
    char r[32] = {};
    GetPrivateProfileStringA(sec, "Fullscreen", "Borderless", r, sizeof(r), ini);
    if (g_menu_ini[0]) {  // a choice from the menus, when mcm2max.ini couldn't be written
      char o[32] = {};
      GetPrivateProfileStringA(g_menu_section, "Fullscreen", "", o, sizeof(o), g_menu_ini);
      if (o[0]) std::snprintf(r, sizeof(r), "%s", o);
    }
    r[std::strcspn(r, " \t;")] = 0;
    g_config.exclusive = _strnicmp(r, "Excl", 4) == 0;
    g_config.card_test = GetPrivateProfileIntA(sec, "CardTest", 0, ini) != 0;
    g_config.monitor = GetPrivateProfileIntA(sec, "Monitor", 1, ini);
    if (g_config.monitor < 1 || g_config.monitor > 16) g_config.monitor = 1;
    // BorderlessResolution=Desktop or <w>x<h>; ExclusiveResolution=Desktop or
    // <w>x<h>[@<hz>]. Older inis: Resolution= and FullscreenMode= stand in
    // for them (see MigrateDisplayKeys).
    auto read = [&](const char* key, const char* old_key, int* w, int* h, int* hz) {
      char m[48] = {};
      GetPrivateProfileStringA(sec, key, "", m, sizeof(m), ini);
      if (g_menu_ini[0]) {
        char o[48] = {};
        GetPrivateProfileStringA(g_menu_section, key, "", o, sizeof(o), g_menu_ini);
        if (o[0]) std::snprintf(m, sizeof(m), "%s", o);
      }
      if (!m[0] && old_key) {
        GetPrivateProfileStringA(sec, old_key, "", m, sizeof(m), ini);
        if (m[0]) {
          std::snprintf(g_display_migrate[key == std::string("BorderlessResolution") ? 0 : 1], 48, "%s", m);
        }
      }
      m[std::strcspn(m, " \t;")] = 0;
      int a = 0, b = 0, c = 0;
      *w = *h = 0;
      if (hz) *hz = 0;
      if (std::sscanf(m, "%dx%d@%d", &a, &b, &c) >= 2 && a >= 640 && b >= 480 && a <= 16384 && b <= 16384) {
        *w = a;
        *h = b;
        if (hz) *hz = c > 0 && c <= 1000 ? c : 0;
      }
    };
    read("BorderlessResolution", "Resolution", &g_config.bl_width, &g_config.bl_height, nullptr);
    read("ExclusiveResolution", "FullscreenMode", &g_config.fs_width, &g_config.fs_height, &g_config.fs_hz);
  }
  g_config.renderer_label = GetPrivateProfileIntA(sec, "RendererLabel", 1, ini) != 0;
  g_config.gpu_text = GetPrivateProfileIntA(sec, "GpuText", 1, ini) != 0;
  g_config.hud_text = GetPrivateProfileIntA(sec, "HudText", 1, ini) != 0;
  g_config.hud_scale = ReadFloat("HudScale", 2.0f, ini);
  g_config.label_scale = ReadFloat("LabelScale", 1.5f, ini);
  g_config.line_width = ReadFloat("LineWidth", 3.0f, ini);
  if (!(g_config.line_width >= 1.0f && g_config.line_width <= 16.0f)) g_config.line_width = 3.0f;
  GetPrivateProfileStringA(sec, "HudFont", "FONTS\\ContrailOne-Regular.ttf", g_config.hud_font, sizeof(g_config.hud_font), ini);
  g_config.hud_font[std::strcspn(g_config.hud_font, "\t;")] = 0;  // drop an inline comment
  for (size_t n = std::strlen(g_config.hud_font); n && g_config.hud_font[n - 1] == ' ';) g_config.hud_font[--n] = 0;
  GetPrivateProfileStringA(sec, "HudDigitFont", "FONTS\\DSEG7Classic-BoldItalic.ttf", g_config.hud_digit_font, sizeof(g_config.hud_digit_font), ini);
  g_config.hud_digit_font[std::strcspn(g_config.hud_digit_font, "\t;")] = 0;
  for (size_t n = std::strlen(g_config.hud_digit_font); n && g_config.hud_digit_font[n - 1] == ' ';)
    g_config.hud_digit_font[--n] = 0;
  g_config.hud_stunt_text_size = ReadFloat("HudStuntTextSize", 0.8f, ini);
  if (!(g_config.hud_stunt_text_size >= 0.5f && g_config.hud_stunt_text_size <= 1.5f)) g_config.hud_stunt_text_size = 0.8f;
  g_config.hud_digit_size = ReadFloat("HudDigitSize", 0.7f, ini);
  if (!(g_config.hud_digit_size >= 0.5f && g_config.hud_digit_size <= 2.0f)) g_config.hud_digit_size = 0.7f;
  g_config.race_title = GetPrivateProfileIntA(sec, "RaceTitle", 1, ini) != 0;
  g_config.race_title_size = ReadFloat("RaceTitleSize", 1.0f, ini);
  if (!(g_config.race_title_size >= 0.5f && g_config.race_title_size <= 2.0f)) g_config.race_title_size = 1.0f;
  g_config.announcer = GetPrivateProfileIntA(sec, "Announcer", 1, ini) != 0;
  g_config.announcer_volume = GetPrivateProfileIntA(sec, "AnnouncerVolume", 100, ini);
  if (g_config.announcer_volume < 0) g_config.announcer_volume = 0;
  if (g_config.announcer_volume > 100) g_config.announcer_volume = 100;
  g_config.race_title_log = GetPrivateProfileIntA(sec, "RaceTitleLog", 0, ini) != 0;
  g_config.text_log = GetPrivateProfileIntA(sec, "TextLog", 0, ini) != 0;
  g_config.gdi_log = GetPrivateProfileIntA(sec, "GdiLog", 0, ini) != 0;
  g_config.d3d9_log = GetPrivateProfileIntA(sec, "D3D9Log", 0, ini) != 0;
  g_config.activate_log = GetPrivateProfileIntA(sec, "ActivateLog", 0, ini) != 0;

  float fov = ReadFloat("Fov", 0.0f, ini);
  g_config.fov = fov >= 20.0f && fov <= 150.0f ? fov : 0.0f;
  g_config.anisotropy = Clamp(ReadMenuInt("Anisotropy", GetPrivateProfileIntA(sec, "Anisotropy", 16, ini)), 0, 16);
  g_config.msaa = Clamp(ReadMenuInt("Antialiasing", GetPrivateProfileIntA(sec, "Antialiasing", 4, ini)), 0, 8);
  float bias = ReadFloat("MipBias", 0.0f, ini);
  g_config.mip_bias = bias < -4.0f ? -4.0f : bias > 4.0f ? 4.0f : bias;
  float fog = ReadFloat("FogDistance", 1.0f, ini);
  g_config.fog_scale = fog >= 0.1f && fog <= 20.0f ? fog : 1.0f;
  float view = ReadFloat("ViewDistance", 1.0f, ini);
  if (g_config.menu_options && g_menu_ini[0]) view = ReadFloat("ViewDistance", view, g_menu_ini);
  g_config.view_distance = view >= 0.25f && view <= 10.0f ? view : 1.0f;
  float tdet = ReadFloat("TerrainDetail", 2.0f, ini);
  g_config.terrain_detail = tdet >= 0.25f && tdet <= 8.0f ? tdet : 2.0f;
  g_config.tree_always_near = ReadFloat("TreeDetailDistance", 1.0f, ini) == 0.0f;
  float lod = ReadFloat("ModelLod", 1.0f, ini);
  g_config.model_lod = lod >= 0.25f && lod <= 10.0f ? lod : 1.0f;
  g_config.texture_replace = ReadMenuInt("TextureReplace", GetPrivateProfileIntA(sec, "TextureReplace", 1, ini)) != 0;
  g_config.texture_dump = GetPrivateProfileIntA(sec, "TextureDump", 0, ini) != 0;
  g_config.texture_decode_thread = GetPrivateProfileIntA(sec, "TextureDecodeThread", 1, ini) != 0;
  g_config.texture_preload = GetPrivateProfileIntA(sec, "TexturePreload", 1, ini) != 0;
  g_config.texture_preload_mb = GetPrivateProfileIntA(sec, "TexturePreloadMB", 256, ini);
  if (g_config.texture_preload_mb < 0) g_config.texture_preload_mb = 0;
  if (g_config.texture_preload_mb > 1024) g_config.texture_preload_mb = 1024;
  g_config.true_color_textures = GetPrivateProfileIntA(sec, "TrueColorTextures", 1, ini) != 0;
  g_config.texture_log = GetPrivateProfileIntA(sec, "TextureLog", 0, ini) != 0;
}
