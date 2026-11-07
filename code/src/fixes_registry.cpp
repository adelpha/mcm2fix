// Keep the game's settings out of HKEY_LOCAL_MACHINE.
//
// Every registry access in Mcm2.exe goes through RegOpenKeyExA /
// RegCreateKeyExA on HKLM\Software\Microsoft\Microsoft Games\Motocross
// Madness 2, opened with KEY_ALL_ACCESS for writes. A standard user can't
// write there; Windows only made it work through registry virtualization.
// We hook those two imports and send that key to
// HKCU\Software\mcm2fix\Motocross Madness 2 instead. On first run the new
// key is seeded with the install values the game expects, pointing at the
// folder the exe runs from, so no installer is needed.
#include <windows.h>
#include <cstring>
#include <string>

#include "patch.h"

namespace {

constexpr uintptr_t kIatRegOpenKeyExA = 0x555008;
constexpr uintptr_t kIatRegCreateKeyExA = 0x55500C;

const char kGameKey[] = "Software\\Microsoft\\Microsoft Games\\Motocross Madness 2";
const char kUserKey[] = "Software\\mcm2fix\\Motocross Madness 2";

using RegOpenKeyExAFn = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegCreateKeyExAFn = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
                                        const SECURITY_ATTRIBUTES*, PHKEY, LPDWORD);
RegOpenKeyExAFn g_open = nullptr;
RegCreateKeyExAFn g_create = nullptr;

// If (root, subkey) names the game's key or a key below it, rewrite it to
// the per-user key. Returns false if it is some other key.
bool Redirect(HKEY root, LPCSTR subkey, std::string* out) {
  if (root != HKEY_LOCAL_MACHINE || subkey == nullptr) return false;
  const size_t n = sizeof(kGameKey) - 1;
  if (_strnicmp(subkey, kGameKey, n) != 0) return false;
  if (subkey[n] != '\0' && subkey[n] != '\\') return false;
  *out = kUserKey;
  *out += subkey + n;
  return true;
}

LONG WINAPI HookedRegOpenKeyExA(HKEY root, LPCSTR subkey, DWORD options, REGSAM sam,
                                PHKEY result) {
  std::string user;
  if (Redirect(root, subkey, &user))
    return g_open(HKEY_CURRENT_USER, user.c_str(), options, sam, result);
  return g_open(root, subkey, options, sam, result);
}

LONG WINAPI HookedRegCreateKeyExA(HKEY root, LPCSTR subkey, DWORD reserved, LPSTR cls,
                                  DWORD options, REGSAM sam, const SECURITY_ATTRIBUTES* sa,
                                  PHKEY result, LPDWORD disposition) {
  std::string user;
  if (Redirect(root, subkey, &user))
    return g_create(HKEY_CURRENT_USER, user.c_str(), reserved, cls, options, sam, sa, result,
                    disposition);
  return g_create(root, subkey, reserved, cls, options, sam, sa, result, disposition);
}

std::string GameFolder() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  char* slash = std::strrchr(path, '\\');
  if (slash) *slash = '\0';
  return path;
}

void SetString(HKEY key, const char* name, const std::string& value) {
  RegSetValueExA(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 static_cast<DWORD>(value.size() + 1));
}

void SetDword(HKEY key, const char* name, DWORD value) {
  RegSetValueExA(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

bool HasValue(HKEY key, const char* name) {
  return RegQueryValueExA(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// Seed the values the installer used to write. Existing values are kept,
// except the paths, which always follow the folder the exe runs from.
void SeedUserKey() {
  HKEY key;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, kUserKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    patch::Log("registry: cannot create HKCU\\%s", kUserKey);
    return;
  }
  const std::string folder = GameFolder();
  SetString(key, "HardDriveRootPath", folder);
  SetString(key, "CDPath", folder + "\\");  // no-CD build: the "CD" is the game folder
  if (!HasValue(key, "InstallType")) SetString(key, "InstallType", "Full");
  if (!HasValue(key, "VersionType")) SetString(key, "VersionType", "RetailVersion");
  if (!HasValue(key, "InstalledGroup")) SetString(key, "InstalledGroup", "3");
  if (!HasValue(key, "Launched")) SetString(key, "Launched", "1");
  if (!HasValue(key, "LangID")) SetDword(key, "LangID", 9);
  if (!HasValue(key, "AllowFreezeCamera")) SetDword(key, "AllowFreezeCamera", 1);

  // Carry over the CD key from an existing install, if there is one.
  if (!HasValue(key, "PID")) {
    HKEY old;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kGameKey, 0, KEY_READ, &old) == ERROR_SUCCESS) {
      char pid[64] = {};
      DWORD size = sizeof(pid) - 1, type = 0;
      if (RegQueryValueExA(old, "PID", nullptr, &type, reinterpret_cast<BYTE*>(pid), &size) ==
              ERROR_SUCCESS &&
          type == REG_SZ)
        SetString(key, "PID", pid);
      RegCloseKey(old);
    }
  }
  RegCloseKey(key);
  patch::Log("registry: settings in HKCU\\%s, game folder %s", kUserKey, folder.c_str());
}

}  // namespace

void ApplyRegistryFixes() {
  SeedUserKey();
  g_open = reinterpret_cast<RegOpenKeyExAFn>(
      patch::HookIat(kIatRegOpenKeyExA, reinterpret_cast<void*>(&HookedRegOpenKeyExA)));
  g_create = reinterpret_cast<RegCreateKeyExAFn>(
      patch::HookIat(kIatRegCreateKeyExA, reinterpret_cast<void*>(&HookedRegCreateKeyExA)));
}
