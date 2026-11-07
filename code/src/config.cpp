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
  g_config.one_core = GetPrivateProfileIntA("mcm2fix", "OneCore", 1, ini) != 0;
  g_config.log = GetPrivateProfileIntA("mcm2fix", "Log", 1, ini) != 0;
}
