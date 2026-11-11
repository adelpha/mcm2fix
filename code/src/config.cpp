#include "config.h"
#include "xp_compat.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
}

const Config& GetConfig() { return g_config; }

void LoadConfig() {
  char ini[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, ini, MAX_PATH);
  if (char* slash = std::strrchr(ini, '\\')) slash[1] = '\0';
  xp::Append(ini, "mcm2fix.ini");
  g_config.one_core = GetPrivateProfileIntA("mcm2fix", "OneCore", 0, ini) != 0;
  g_config.log = GetPrivateProfileIntA("mcm2fix", "Log", 1, ini) != 0;
  g_config.startup = GetPrivateProfileIntA("mcm2fix", "Startup", 1, ini) != 0;
  g_config.registry = GetPrivateProfileIntA("mcm2fix", "Registry", 1, ini) != 0;
  g_config.files = GetPrivateProfileIntA("mcm2fix", "Files", 1, ini) != 0;
  g_config.reg_log = GetPrivateProfileIntA("mcm2fix", "RegLog", 0, ini) != 0;
  g_config.wide_fov = GetPrivateProfileIntA("mcm2fix", "WideFov", 1, ini) != 0;
  g_config.alpha_cut = GetPrivateProfileIntA("mcm2fix", "AlphaCut", 128, ini);
  g_config.alpha_log = GetPrivateProfileIntA("mcm2fix", "AlphaLog", 0, ini) != 0;
  g_config.vsync = GetPrivateProfileIntA("mcm2fix", "VSync", 1, ini) != 0;
  g_config.frame_cap = GetPrivateProfileIntA("mcm2fix", "FrameCap", 0, ini);
  if (g_config.frame_cap < 0) g_config.frame_cap = 0;
  g_config.precise_timer = GetPrivateProfileIntA("mcm2fix", "PreciseTimer", 1, ini) != 0;
  g_config.frame_fix = GetPrivateProfileIntA("mcm2fix", "FrameFix", 1, ini) != 0;
  g_config.ref_rate = GetPrivateProfileIntA("mcm2fix", "RefRate", 30, ini);
  if (g_config.ref_rate < 10 || g_config.ref_rate > 240) g_config.ref_rate = 30;
  g_config.ai_rate = GetPrivateProfileIntA("mcm2fix", "AiRate", 30, ini);
  if (g_config.ai_rate < 0) g_config.ai_rate = 0;
  g_config.ai_steer_rate = GetPrivateProfileIntA("mcm2fix", "AiSteerRate", 0, ini);
  if (g_config.ai_steer_rate < 0) g_config.ai_steer_rate = 0;
  g_config.smooth_log = GetPrivateProfileIntA("mcm2fix", "SmoothLog", 0, ini);
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
  g_config.async_sound = GetPrivateProfileIntA("mcm2fix", "AsyncSound", 2, ini);
  g_config.sound_max_distance_mute = GetPrivateProfileIntA("mcm2fix", "SoundMaxDistanceMute", 1, ini) != 0;
  g_config.sound_log = GetPrivateProfileIntA("mcm2fix", "SoundLog", 0, ini) != 0;
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
  g_config.text_log = GetPrivateProfileIntA("mcm2fix", "TextLog", 0, ini) != 0;
  g_config.gdi_log = GetPrivateProfileIntA("mcm2fix", "GdiLog", 0, ini) != 0;
  g_config.d3d9_log = GetPrivateProfileIntA("mcm2fix", "D3D9Log", 0, ini) != 0;
  g_config.activate_log = GetPrivateProfileIntA("mcm2fix", "ActivateLog", 0, ini) != 0;

  float fov = ReadFloat("Fov", 0.0f, ini);
  g_config.fov = fov >= 20.0f && fov <= 150.0f ? fov : 0.0f;
  g_config.anisotropy = Clamp(GetPrivateProfileIntA("mcm2fix", "Anisotropy", 16, ini), 0, 16);
  g_config.msaa = Clamp(GetPrivateProfileIntA("mcm2fix", "Antialiasing", 4, ini), 0, 8);
  float bias = ReadFloat("MipBias", 0.0f, ini);
  g_config.mip_bias = bias < -4.0f ? -4.0f : bias > 4.0f ? 4.0f : bias;
  float fog = ReadFloat("FogDistance", 1.0f, ini);
  g_config.fog_scale = fog >= 0.1f && fog <= 20.0f ? fog : 1.0f;
  float view = ReadFloat("ViewDistance", 1.0f, ini);
  g_config.view_distance = view >= 0.25f && view <= 10.0f ? view : 1.0f;
  g_config.tree_always_near = ReadFloat("TreeDetailDistance", 1.0f, ini) == 0.0f;
  float lod = ReadFloat("ModelLod", 1.0f, ini);
  g_config.model_lod = lod >= 0.25f && lod <= 10.0f ? lod : 1.0f;
  g_config.texture_replace = GetPrivateProfileIntA("mcm2fix", "TextureReplace", 1, ini) != 0;
  g_config.texture_dump = GetPrivateProfileIntA("mcm2fix", "TextureDump", 0, ini) != 0;
  g_config.true_color_textures = GetPrivateProfileIntA("mcm2fix", "TrueColorTextures", 1, ini) != 0;
  g_config.texture_log = GetPrivateProfileIntA("mcm2fix", "TextureLog", 0, ini) != 0;
}
