#include "config.h"

#include <windows.h>
#include <cstring>

namespace {
Config g_config;
}

const Config& GetConfig() { return g_config; }

void LoadConfig() {
  char ini[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, ini, MAX_PATH);
  if (char* slash = std::strrchr(ini, '\\')) slash[1] = '\0';
  strncat_s(ini, "mcm2fix.ini", _TRUNCATE);
  g_config.one_core = GetPrivateProfileIntA("mcm2fix", "OneCore", 0, ini) != 0;
  g_config.log = GetPrivateProfileIntA("mcm2fix", "Log", 1, ini) != 0;
  g_config.startup = GetPrivateProfileIntA("mcm2fix", "Startup", 1, ini) != 0;
  g_config.registry = GetPrivateProfileIntA("mcm2fix", "Registry", 1, ini) != 0;
  g_config.files = GetPrivateProfileIntA("mcm2fix", "Files", 1, ini) != 0;
  g_config.reg_log = GetPrivateProfileIntA("mcm2fix", "RegLog", 0, ini) != 0;
  g_config.wide_fov = GetPrivateProfileIntA("mcm2fix", "WideFov", 1, ini) != 0;
  g_config.alpha_cut = GetPrivateProfileIntA("mcm2fix", "AlphaCut", 128, ini);
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
  g_config.sim_rate = GetPrivateProfileIntA("mcm2fix", "SimRate", 30, ini);
  if (g_config.sim_rate < 0) g_config.sim_rate = 0;
  if (g_config.sim_rate > 0 && g_config.sim_rate < 10) g_config.sim_rate = 10;
  if (g_config.sim_rate > 1000) g_config.sim_rate = 1000;
  g_config.dd_stats = GetPrivateProfileIntA("mcm2fix", "DDStats", 0, ini) != 0;
  g_config.activate_log = GetPrivateProfileIntA("mcm2fix", "ActivateLog", 0, ini) != 0;
}
