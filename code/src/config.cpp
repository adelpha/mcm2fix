#include "config.h"
#include "xp_compat.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
Config g_config;

float ReadFloat(const char* key, float def, const char* ini) {
  char buf[64] = {};
  GetPrivateProfileStringA("mcm2fix", key, "", buf, sizeof(buf), ini);
  if (!buf[0]) return def;
  char* end = nullptr;
  double v = std::strtod(buf, &end);
  return end != buf ? static_cast<float>(v) : def;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

char g_menu_ini[MAX_PATH] = {};
char g_game_ini[MAX_PATH] = {};  // mcm2fix.ini next to Mcm2.exe

// A key set from the menus (mcm2fix-menu.ini) wins over mcm2fix.ini.
int ReadMenuInt(const char* key, int value) {
  if (!g_config.menu_options || !g_menu_ini[0]) return value;
  return GetPrivateProfileIntA("mcm2fix", key, value, g_menu_ini);
}
}

const char* MenuIniPath() { return g_menu_ini; }

int MenuOverride(const char* key, int value) { return ReadMenuInt(key, value); }

const char* SetFromMenu(const char* key, int value) {
  if (!std::strcmp(key, "MotionBlur")) g_config.motion_blur = value != 0;
  else if (!std::strcmp(key, "VSync")) g_config.vsync = value != 0;
  else if (!std::strcmp(key, "AlphaToCoverage")) g_config.alpha_to_coverage = value;
  else if (!std::strcmp(key, "Anisotropy")) g_config.anisotropy = value;
  char v[16];
  std::snprintf(v, sizeof(v), "%d", value);
  // mcm2fix.ini itself when it can be written (only that key's line changes);
  // then any older choice for the key in the per-user file is dropped so it
  // can't win at the next start. Otherwise the per-user file.
  const DWORD attr = g_game_ini[0] ? GetFileAttributesA(g_game_ini) : INVALID_FILE_ATTRIBUTES;
  if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_READONLY) &&
      WritePrivateProfileStringA("mcm2fix", key, v, g_game_ini)) {
    if (g_menu_ini[0] && GetFileAttributesA(g_menu_ini) != INVALID_FILE_ATTRIBUTES)
      WritePrivateProfileStringA("mcm2fix", key, nullptr, g_menu_ini);
    return g_game_ini;
  }
  if (!g_menu_ini[0]) return "(nowhere: no per-user folder)";
  CreateDirectoryA(xp::LocalAppData().append("\\Motocross Madness 2").c_str(), nullptr);
  WritePrivateProfileStringA("mcm2fix", key, v, g_menu_ini);
  return g_menu_ini;
}

const Config& GetConfig() { return g_config; }

void LoadConfig() {
  char ini[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, ini, MAX_PATH);
  if (char* slash = std::strrchr(ini, '\\')) slash[1] = '\0';
  xp::Append(ini, "mcm2fix.ini");
  xp::Copy(g_game_ini, ini);
  // First: the menus' saved choices (ReadMenuInt) need these.
  g_config.menu_options = GetPrivateProfileIntA("mcm2fix", "MenuOptions", 1, ini) != 0;
  {
    const std::string dir = xp::LocalAppData();
    if (!dir.empty()) xp::Copy(g_menu_ini, (dir + "\\Motocross Madness 2\\mcm2fix-menu.ini").c_str());
  }
  g_config.one_core = GetPrivateProfileIntA("mcm2fix", "OneCore", 0, ini) != 0;
  g_config.log = GetPrivateProfileIntA("mcm2fix", "Log", 1, ini) != 0;
  g_config.startup = GetPrivateProfileIntA("mcm2fix", "Startup", 1, ini) != 0;
  g_config.registry = GetPrivateProfileIntA("mcm2fix", "Registry", 1, ini) != 0;
  g_config.files = GetPrivateProfileIntA("mcm2fix", "Files", 1, ini) != 0;
  g_config.reg_log = GetPrivateProfileIntA("mcm2fix", "RegLog", 0, ini) != 0;
  g_config.wide_fov = ReadMenuInt("WideFov", GetPrivateProfileIntA("mcm2fix", "WideFov", 1, ini)) != 0;
  g_config.alpha_cut = GetPrivateProfileIntA("mcm2fix", "AlphaCut", 128, ini);
  g_config.menu_log = GetPrivateProfileIntA("mcm2fix", "MenuLog", 0, ini) != 0;
  g_config.menu_flag = GetPrivateProfileIntA("mcm2fix", "MenuFlag", 1, ini) != 0;
  {
    auto pct = [&](const char* key, int def, int hi) {
      const int v = static_cast<int>(GetPrivateProfileIntA("mcm2fix", key, def, ini));
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
    GetPrivateProfileStringA("mcm2fix", "MenuBackground", "UI\\MAINMENU\\01_MAIN\\BackGround_01.png", v, sizeof(v), ini);
    if (char* semi = std::strchr(v, ';')) *semi = '\0';
    size_t n = std::strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = '\0';
    const char* start = v;
    while (*start == ' ' || *start == '\t') ++start;
    xp::Copy(g_config.menu_background, *start ? start : "UI\\MAINMENU\\01_MAIN\\BackGround_01.png");
  }
  g_config.motion_blur = ReadMenuInt("MotionBlur", GetPrivateProfileIntA("mcm2fix", "MotionBlur", 1, ini)) != 0;
  g_config.motion_blur_strength = Clamp(GetPrivateProfileIntA("mcm2fix", "MotionBlurStrength", 80, ini), 0, 300);
  g_config.motion_blur_min_speed = Clamp(GetPrivateProfileIntA("mcm2fix", "MotionBlurMinSpeed", 30, ini), 0, 200);
  g_config.motion_blur_max_speed = Clamp(GetPrivateProfileIntA("mcm2fix", "MotionBlurMaxSpeed", 70, ini), 1, 250);
  if (g_config.motion_blur_max_speed <= g_config.motion_blur_min_speed)
    g_config.motion_blur_max_speed = g_config.motion_blur_min_speed + 1;
  g_config.motion_blur_center = Clamp(GetPrivateProfileIntA("mcm2fix", "MotionBlurCenter", 60, ini), 0, 95);
  g_config.motion_blur_log = GetPrivateProfileIntA("mcm2fix", "MotionBlurLog", 0, ini) != 0;
  g_config.alpha_to_coverage = ReadMenuInt("AlphaToCoverage", GetPrivateProfileIntA("mcm2fix", "AlphaToCoverage", 2, ini));
  if (g_config.alpha_to_coverage < 0 || g_config.alpha_to_coverage > 2) g_config.alpha_to_coverage = 2;
  g_config.alpha_log = GetPrivateProfileIntA("mcm2fix", "AlphaLog", 0, ini) != 0;
  g_config.vsync = ReadMenuInt("VSync", GetPrivateProfileIntA("mcm2fix", "VSync", 1, ini)) != 0;
  g_config.frame_cap = GetPrivateProfileIntA("mcm2fix", "FrameCap", 0, ini);
  if (g_config.frame_cap < 0) g_config.frame_cap = 0;
  g_config.precise_timer = GetPrivateProfileIntA("mcm2fix", "PreciseTimer", 1, ini) != 0;
  g_config.frame_fix = GetPrivateProfileIntA("mcm2fix", "FrameFix", 1, ini) != 0;
  g_config.ref_rate = GetPrivateProfileIntA("mcm2fix", "RefRate", 30, ini);
  if (g_config.ref_rate < 10 || g_config.ref_rate > 240) g_config.ref_rate = 30;
  g_config.ai_rate = GetPrivateProfileIntA("mcm2fix", "AiRate", 30, ini);
  if (g_config.ai_rate < 0) g_config.ai_rate = 0;
  {
    static const char* kLevel[3] = {"Easy", "Normal", "Hard"};
    static const int kAccel[3] = {80, 130, 150};
    static const int kTop[3] = {90, 100, 100};
    for (int d = 0; d < 3; ++d) {
      char key[32];
      std::snprintf(key, sizeof(key), "AiAccel%s", kLevel[d]);
      int v = GetPrivateProfileIntA("mcm2fix", key, kAccel[d], ini);
      g_config.ai_accel[d] = v < 10 ? 10 : v > 400 ? 400 : v;
      std::snprintf(key, sizeof(key), "AiTopSpeed%s", kLevel[d]);
      v = GetPrivateProfileIntA("mcm2fix", key, kTop[d], ini);
      g_config.ai_top_speed[d] = v < 50 ? 50 : v > 150 ? 150 : v;
    }
    g_config.ai_log = GetPrivateProfileIntA("mcm2fix", "AiLog", 0, ini) != 0;
  }
  g_config.ai_pick_fix = GetPrivateProfileIntA("mcm2fix", "AiPickFix", 1, ini) != 0;
  g_config.ai_steer_rate = GetPrivateProfileIntA("mcm2fix", "AiSteerRate", 0, ini);
  if (g_config.ai_steer_rate < 0) g_config.ai_steer_rate = 0;
  g_config.smooth_log = GetPrivateProfileIntA("mcm2fix", "SmoothLog", 0, ini);
  g_config.rider_shadows = ReadMenuInt("RiderShadows", GetPrivateProfileIntA("mcm2fix", "RiderShadows", 2, ini));
  g_config.reflections = ReadMenuInt("Reflections", GetPrivateProfileIntA("mcm2fix", "Reflections", 2, ini));
  if (g_config.reflections < 0 || g_config.reflections > 2) g_config.reflections = 2;
  if (g_config.rider_shadows < 0 || g_config.rider_shadows > 2) g_config.rider_shadows = 2;
  g_config.shadow_strength = GetPrivateProfileIntA("mcm2fix", "ShadowStrength", 0, ini);
  if (g_config.shadow_strength < 0 || g_config.shadow_strength > 100) g_config.shadow_strength = 0;
  g_config.shadow_softness = GetPrivateProfileIntA("mcm2fix", "ShadowSoftness", 100, ini);
  if (g_config.shadow_softness < 0 || g_config.shadow_softness > 400) g_config.shadow_softness = 100;
  g_config.shadow_log = GetPrivateProfileIntA("mcm2fix", "ShadowLog", 0, ini);
  g_config.sim_smooth = GetPrivateProfileIntA("mcm2fix", "SimSmooth", 1, ini) != 0;
  g_config.sim_rate = GetPrivateProfileIntA("mcm2fix", "SimRate", 30, ini);
  if (g_config.sim_rate < 0) g_config.sim_rate = 0;
  if (g_config.sim_rate > 0 && g_config.sim_rate < 10) g_config.sim_rate = 10;
  if (g_config.sim_rate > 1000) g_config.sim_rate = 1000;
  g_config.arrow_heading = GetPrivateProfileIntA("mcm2fix", "ArrowFix", 1, ini) != 0;
  char buf[32] = {};
  GetPrivateProfileStringA("mcm2fix", "ArrowScale", "2.0", buf, sizeof(buf), ini);
  float scale = static_cast<float>(std::atof(buf));
  g_config.arrow_scale = (scale >= 0.25f && scale <= 8.0f) ? scale : 2.0f;
  GetPrivateProfileStringA("mcm2fix", "ArrowTurn", "0.8", buf, sizeof(buf), ini);
  float turn = static_cast<float>(std::atof(buf));
  g_config.arrow_turn = (turn >= 0.0f && turn <= 2.0f) ? turn : 0.8f;
  GetPrivateProfileStringA("mcm2fix", "ArrowTilt", "10", buf, sizeof(buf), ini);
  float tilt = static_cast<float>(std::atof(buf));
  g_config.arrow_log = GetPrivateProfileIntA("mcm2fix", "ArrowLog", 0, ini) != 0;
  g_config.fov_log = GetPrivateProfileIntA("mcm2fix", "FovLog", 0, ini) != 0;
  g_config.arrow_tilt = (tilt >= -45.0f && tilt <= 45.0f) ? tilt : 10.0f;
  g_config.restart_fix = GetPrivateProfileIntA("mcm2fix", "RestartFix", 1, ini) != 0;
  g_config.restart_log = GetPrivateProfileIntA("mcm2fix", "RestartLog", 0, ini) != 0;
  g_config.profile = GetPrivateProfileIntA("mcm2fix", "Profile", 0, ini) != 0;
  g_config.profile_sample_us = GetPrivateProfileIntA("mcm2fix", "ProfileSampleUs", 1000, ini);
  g_config.fast_crt = GetPrivateProfileIntA("mcm2fix", "FastCrt", 1, ini) != 0;
  g_config.simd = GetPrivateProfileIntA("mcm2fix", "Simd", 1, ini) != 0;
  g_config.simd_log = GetPrivateProfileIntA("mcm2fix", "SimdLog", 0, ini) != 0;
  g_config.pause_on_focus_loss = GetPrivateProfileIntA("mcm2fix", "PauseOnFocusLoss", 1, ini) != 0;
  g_config.dpi_aware = GetPrivateProfileIntA("mcm2fix", "DpiAware", 1, ini) != 0;
  g_config.race_power = GetPrivateProfileIntA("mcm2fix", "RacePower", 1, ini);
  g_config.race_power_async = GetPrivateProfileIntA("mcm2fix", "RacePowerAsync", 1, ini);
  g_config.fast_game_code = GetPrivateProfileIntA("mcm2fix", "FastGameCode", 1, ini) != 0;
  g_config.async_sound = GetPrivateProfileIntA("mcm2fix", "AsyncSound", 2, ini);
  g_config.sound_max_distance_mute = GetPrivateProfileIntA("mcm2fix", "SoundMaxDistanceMute", 1, ini);
  g_config.fast_sort = GetPrivateProfileIntA("mcm2fix", "FastSort", 1, ini) != 0;
  g_config.tree_cache_mb = GetPrivateProfileIntA("mcm2fix", "TreeCacheMB", 32, ini);
  if (g_config.tree_cache_mb < 0) g_config.tree_cache_mb = 0;
  if (g_config.tree_cache_mb > 512) g_config.tree_cache_mb = 512;
  g_config.sort_log = GetPrivateProfileIntA("mcm2fix", "SortLog", 0, ini) != 0;
  g_config.input_timing = GetPrivateProfileIntA("mcm2fix", "InputTiming", 1, ini) != 0;
  g_config.alt_tab_fix = GetPrivateProfileIntA("mcm2fix", "AltTabFix", 1, ini) != 0;
  g_config.input_log = GetPrivateProfileIntA("mcm2fix", "InputLog", 0, ini) != 0;
  g_config.sound_log = GetPrivateProfileIntA("mcm2fix", "SoundLog", 0, ini) != 0;
  {
    // Event numbers as in game+0x2d74/0x2d78: 0 Quarry, 1 Baja, 2 Nationals, 3 Supercross, 4 Moto-Tag, 5 Enduro.
    const char* keys[6] = {"QuarryReverb", "BajaReverb", "NationalsReverb", "SupercrossReverb", "TagReverb", "EnduroReverb"};
    const char* defs[6] = {"Mountains", "Mountains", "Forest", "Default", "Default", "Forest"};
    for (int e = 0; e < 6; ++e) {
      char* v = g_config.outdoor_reverb[e];
      if (!keys[e]) continue;
      GetPrivateProfileStringA("mcm2fix", keys[e], defs[e], v, sizeof(g_config.outdoor_reverb[e]), ini);
      v[std::strcspn(v, "\t; ")] = 0;  // drop an inline comment
    }
  }
  g_config.background_mute = GetPrivateProfileIntA("mcm2fix", "BackgroundMute", 1, ini) != 0;
  g_config.sound_pause_log = GetPrivateProfileIntA("mcm2fix", "SoundPauseLog", 0, ini) != 0;
  g_config.state_cache = GetPrivateProfileIntA("mcm2fix", "StateCache", 1, ini) != 0;
  g_config.dd_stats = GetPrivateProfileIntA("mcm2fix", "DDStats", 0, ini) != 0;
  g_config.draw_batch = GetPrivateProfileIntA("mcm2fix", "DrawBatch", 1, ini) != 0;
  g_config.batch_log = GetPrivateProfileIntA("mcm2fix", "BatchLog", 0, ini) != 0;
  g_config.draw_log = GetPrivateProfileIntA("mcm2fix", "DrawLog", 0, ini) != 0;
  {
    char r[32] = {};
    GetPrivateProfileStringA("mcm2fix", "Renderer", "D3D11", r, sizeof(r), ini);
    // Windows keeps an inline "; comment" in the value, so only the first
    // word counts. Accepts D3D9 / DX9 / 9 and D3D11 / DX11 / 11.
    r[std::strcspn(r, " \t;")] = 0;
    g_config.renderer = (std::strstr(r, "9") && !std::strstr(r, "11")) ? 9 : 11;
  }
  {
    char r[32] = {};
    GetPrivateProfileStringA("mcm2fix", "Fullscreen", "Borderless", r, sizeof(r), ini);
    r[std::strcspn(r, " \t;")] = 0;
    g_config.exclusive = _strnicmp(r, "Excl", 4) == 0;
    // FullscreenMode=Desktop or <width>x<height>[@<hz>], e.g. 1024x768@85.
    char m[48] = {};
    GetPrivateProfileStringA("mcm2fix", "FullscreenMode", "Desktop", m, sizeof(m), ini);
    m[std::strcspn(m, " \t;")] = 0;
    int w = 0, h = 0, hz = 0;
    if (std::sscanf(m, "%dx%d@%d", &w, &h, &hz) >= 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384) {
      g_config.fs_width = w;
      g_config.fs_height = h;
      g_config.fs_hz = hz > 0 && hz <= 1000 ? hz : 0;
    }
  }
  g_config.renderer_label = GetPrivateProfileIntA("mcm2fix", "RendererLabel", 1, ini) != 0;
  g_config.gpu_text = GetPrivateProfileIntA("mcm2fix", "GpuText", 1, ini) != 0;
  g_config.hud_text = GetPrivateProfileIntA("mcm2fix", "HudText", 1, ini) != 0;
  g_config.hud_scale = ReadFloat("HudScale", 2.0f, ini);
  g_config.label_scale = ReadFloat("LabelScale", 1.5f, ini);
  g_config.line_width = ReadFloat("LineWidth", 3.0f, ini);
  if (!(g_config.line_width >= 1.0f && g_config.line_width <= 16.0f)) g_config.line_width = 3.0f;
  GetPrivateProfileStringA("mcm2fix", "HudFont", "FONTS\\ContrailOne-Regular.ttf", g_config.hud_font, sizeof(g_config.hud_font), ini);
  g_config.hud_font[std::strcspn(g_config.hud_font, "\t;")] = 0;  // drop an inline comment
  for (size_t n = std::strlen(g_config.hud_font); n && g_config.hud_font[n - 1] == ' ';) g_config.hud_font[--n] = 0;
  GetPrivateProfileStringA("mcm2fix", "HudDigitFont", "FONTS\\DSEG7Classic-BoldItalic.ttf", g_config.hud_digit_font, sizeof(g_config.hud_digit_font), ini);
  g_config.hud_digit_font[std::strcspn(g_config.hud_digit_font, "\t;")] = 0;
  for (size_t n = std::strlen(g_config.hud_digit_font); n && g_config.hud_digit_font[n - 1] == ' ';)
    g_config.hud_digit_font[--n] = 0;
  g_config.hud_stunt_text_size = ReadFloat("HudStuntTextSize", 0.8f, ini);
  if (!(g_config.hud_stunt_text_size >= 0.5f && g_config.hud_stunt_text_size <= 1.5f)) g_config.hud_stunt_text_size = 0.8f;
  g_config.hud_digit_size = ReadFloat("HudDigitSize", 0.7f, ini);
  if (!(g_config.hud_digit_size >= 0.5f && g_config.hud_digit_size <= 2.0f)) g_config.hud_digit_size = 0.7f;
  g_config.race_title = GetPrivateProfileIntA("mcm2fix", "RaceTitle", 1, ini) != 0;
  g_config.race_title_size = ReadFloat("RaceTitleSize", 1.0f, ini);
  if (!(g_config.race_title_size >= 0.5f && g_config.race_title_size <= 2.0f)) g_config.race_title_size = 1.0f;
  g_config.announcer = GetPrivateProfileIntA("mcm2fix", "Announcer", 1, ini) != 0;
  g_config.announcer_volume = GetPrivateProfileIntA("mcm2fix", "AnnouncerVolume", 100, ini);
  if (g_config.announcer_volume < 0) g_config.announcer_volume = 0;
  if (g_config.announcer_volume > 100) g_config.announcer_volume = 100;
  g_config.race_title_log = GetPrivateProfileIntA("mcm2fix", "RaceTitleLog", 0, ini) != 0;
  g_config.text_log = GetPrivateProfileIntA("mcm2fix", "TextLog", 0, ini) != 0;
  g_config.gdi_log = GetPrivateProfileIntA("mcm2fix", "GdiLog", 0, ini) != 0;
  g_config.d3d9_log = GetPrivateProfileIntA("mcm2fix", "D3D9Log", 0, ini) != 0;
  g_config.activate_log = GetPrivateProfileIntA("mcm2fix", "ActivateLog", 0, ini) != 0;

  float fov = ReadFloat("Fov", 0.0f, ini);
  g_config.fov = fov >= 20.0f && fov <= 150.0f ? fov : 0.0f;
  g_config.anisotropy = Clamp(ReadMenuInt("Anisotropy", GetPrivateProfileIntA("mcm2fix", "Anisotropy", 16, ini)), 0, 16);
  g_config.msaa = Clamp(ReadMenuInt("Antialiasing", GetPrivateProfileIntA("mcm2fix", "Antialiasing", 4, ini)), 0, 8);
  float bias = ReadFloat("MipBias", 0.0f, ini);
  g_config.mip_bias = bias < -4.0f ? -4.0f : bias > 4.0f ? 4.0f : bias;
  float fog = ReadFloat("FogDistance", 1.0f, ini);
  g_config.fog_scale = fog >= 0.1f && fog <= 20.0f ? fog : 1.0f;
  float view = ReadFloat("ViewDistance", 1.0f, ini);
  g_config.view_distance = view >= 0.25f && view <= 10.0f ? view : 1.0f;
  g_config.tree_always_near = ReadFloat("TreeDetailDistance", 1.0f, ini) == 0.0f;
  float lod = ReadFloat("ModelLod", 1.0f, ini);
  g_config.model_lod = lod >= 0.25f && lod <= 10.0f ? lod : 1.0f;
  g_config.texture_replace = ReadMenuInt("TextureReplace", GetPrivateProfileIntA("mcm2fix", "TextureReplace", 1, ini)) != 0;
  g_config.texture_dump = GetPrivateProfileIntA("mcm2fix", "TextureDump", 0, ini) != 0;
  g_config.true_color_textures = GetPrivateProfileIntA("mcm2fix", "TrueColorTextures", 1, ini) != 0;
  g_config.texture_log = GetPrivateProfileIntA("mcm2fix", "TextureLog", 0, ini) != 0;
}
