#include "config.h"

#include <windows.h>
#include <cstdlib>
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
  g_config.arrow_heading = GetPrivateProfileIntA("mcm2fix", "ArrowFix", 1, ini) != 0;
  char buf[32] = {};
  GetPrivateProfileStringA("mcm2fix", "ArrowScale", "2.0", buf, sizeof(buf), ini);
  float scale = static_cast<float>(std::atof(buf));
  g_config.arrow_scale = (scale >= 0.25f && scale <= 8.0f) ? scale : 2.0f;
  GetPrivateProfileStringA("mcm2fix", "ArrowTurn", "0.8", buf, sizeof(buf), ini);
  float turn = static_cast<float>(std::atof(buf));
  g_config.arrow_turn = (turn >= 0.0f && turn <= 2.0f) ? turn : 0.8f;
  g_config.dd_stats = GetPrivateProfileIntA("mcm2fix", "DDStats", 0, ini) != 0;
  g_config.activate_log = GetPrivateProfileIntA("mcm2fix", "ActivateLog", 0, ini) != 0;
}
