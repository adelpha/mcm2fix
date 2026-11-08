#include "config.h"

#include <windows.h>
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
  strncat_s(ini, "mcm2fix.ini", _TRUNCATE);
  g_config.one_core = GetPrivateProfileIntA("mcm2fix", "OneCore", 0, ini) != 0;
  g_config.log = GetPrivateProfileIntA("mcm2fix", "Log", 1, ini) != 0;
  g_config.startup = GetPrivateProfileIntA("mcm2fix", "Startup", 1, ini) != 0;
  g_config.registry = GetPrivateProfileIntA("mcm2fix", "Registry", 1, ini) != 0;
  g_config.files = GetPrivateProfileIntA("mcm2fix", "Files", 1, ini) != 0;
  g_config.reg_log = GetPrivateProfileIntA("mcm2fix", "RegLog", 0, ini) != 0;
  g_config.wide_fov = GetPrivateProfileIntA("mcm2fix", "WideFov", 1, ini) != 0;
  g_config.alpha_cut = GetPrivateProfileIntA("mcm2fix", "AlphaCut", 128, ini);
  g_config.dd_stats = GetPrivateProfileIntA("mcm2fix", "DDStats", 0, ini) != 0;
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
  float lod = ReadFloat("ModelLod", 1.0f, ini);
  g_config.model_lod = lod >= 0.25f && lod <= 10.0f ? lod : 1.0f;
  g_config.texture_replace = GetPrivateProfileIntA("mcm2fix", "TextureReplace", 1, ini) != 0;
  g_config.texture_dump = GetPrivateProfileIntA("mcm2fix", "TextureDump", 0, ini) != 0;
}
