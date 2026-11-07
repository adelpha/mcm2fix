// Diagnostics: log every registry call Mcm2.exe makes (key, value, result,
// data), so runs with and without the HKCU redirect can be compared.
// Installed after the redirect hooks, so it records what the game asked for
// and what it got back.
#include <windows.h>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "patch.h"

namespace {

constexpr uintptr_t kIatRegQueryValueExA = 0x555000;
constexpr uintptr_t kIatRegEnumKeyA = 0x555004;
constexpr uintptr_t kIatRegOpenKeyExA = 0x555008;
constexpr uintptr_t kIatRegCreateKeyExA = 0x55500C;
constexpr uintptr_t kIatRegSetValueExA = 0x555010;
constexpr uintptr_t kIatRegQueryInfoKeyA = 0x555014;
constexpr uintptr_t kIatRegCloseKey = 0x555018;
constexpr uintptr_t kIatRegDeleteKeyA = 0x555020;

decltype(&RegQueryValueExA) g_query;
decltype(&RegEnumKeyA) g_enum;
decltype(&RegOpenKeyExA) g_open;
decltype(&RegCreateKeyExA) g_create;
decltype(&RegSetValueExA) g_set;
decltype(&RegQueryInfoKeyA) g_info;
decltype(&RegCloseKey) g_close;
decltype(&RegDeleteKeyA) g_delete;

std::mutex g_mutex;
std::map<HKEY, std::string> g_names;

std::string Name(HKEY key) {
  if (key == HKEY_LOCAL_MACHINE) return "HKLM";
  if (key == HKEY_CURRENT_USER) return "HKCU";
  if (key == HKEY_CLASSES_ROOT) return "HKCR";
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_names.find(key);
  if (it != g_names.end()) return it->second;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "<%p>", static_cast<void*>(key));
  return buf;
}

void Remember(HKEY key, const std::string& name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_names[key] = name;
}

std::string Data(DWORD type, const BYTE* data, DWORD size) {
  if (!data) return "";
  char buf[200];
  if (type == REG_DWORD && size >= 4) {
    std::snprintf(buf, sizeof(buf), " dword=0x%lx", *reinterpret_cast<const DWORD*>(data));
  } else if (type == REG_SZ) {
    std::snprintf(buf, sizeof(buf), " sz=\"%.120s\"", reinterpret_cast<const char*>(data));
  } else {
    std::snprintf(buf, sizeof(buf), " type=%lu size=%lu", type, size);
  }
  return buf;
}

LONG WINAPI TOpen(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
  LONG r = g_open(root, sub, opt, sam, out);
  std::string name = Name(root) + "\\" + (sub ? sub : "");
  patch::Log("reg open %s sam=%lx -> %ld", name.c_str(), sam, r);
  if (r == ERROR_SUCCESS && out) Remember(*out, name);
  return r;
}

LONG WINAPI TCreate(HKEY root, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
                    LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp) {
  DWORD d = 0;
  LONG r = g_create(root, sub, res, cls, opt, sam, sa, out, disp ? disp : &d);
  std::string name = Name(root) + "\\" + (sub ? sub : "");
  patch::Log("reg create %s sam=%lx -> %ld disp=%lu", name.c_str(), sam, r, disp ? *disp : d);
  if (r == ERROR_SUCCESS && out) Remember(*out, name);
  return r;
}

LONG WINAPI TQuery(HKEY key, LPCSTR value, LPDWORD res, LPDWORD type, LPBYTE data,
                   LPDWORD size) {
  DWORD t = 0;
  LONG r = g_query(key, value, res, type ? type : &t, data, size);
  DWORD tt = type ? *type : t;
  std::string d = (r == ERROR_SUCCESS && size) ? Data(tt, data, *size) : "";
  patch::Log("reg query %s : %s -> %ld%s", Name(key).c_str(), value ? value : "(default)", r,
             d.c_str());
  return r;
}

LONG WINAPI TSet(HKEY key, LPCSTR value, DWORD res, DWORD type, const BYTE* data, DWORD size) {
  LONG r = g_set(key, value, res, type, data, size);
  patch::Log("reg set %s : %s -> %ld%s", Name(key).c_str(), value ? value : "(default)", r,
             Data(type, data, size).c_str());
  return r;
}

LONG WINAPI TEnum(HKEY key, DWORD index, LPSTR name, DWORD size) {
  LONG r = g_enum(key, index, name, size);
  patch::Log("reg enum %s #%lu -> %ld %s", Name(key).c_str(), index, r,
             r == ERROR_SUCCESS && name ? name : "");
  return r;
}

LONG WINAPI TInfo(HKEY key, LPSTR cls, LPDWORD cls_size, LPDWORD res, LPDWORD subkeys,
                  LPDWORD max_sub, LPDWORD max_cls, LPDWORD values, LPDWORD max_name,
                  LPDWORD max_value, LPDWORD sec, PFILETIME time) {
  LONG r = g_info(key, cls, cls_size, res, subkeys, max_sub, max_cls, values, max_name, max_value,
                  sec, time);
  patch::Log("reg info %s -> %ld subkeys=%ld values=%ld", Name(key).c_str(), r,
             subkeys ? static_cast<long>(*subkeys) : -1L, values ? static_cast<long>(*values) : -1L);
  return r;
}

LONG WINAPI TDelete(HKEY key, LPCSTR sub) {
  LONG r = g_delete(key, sub);
  patch::Log("reg delete %s\\%s -> %ld", Name(key).c_str(), sub ? sub : "", r);
  return r;
}

LONG WINAPI TClose(HKEY key) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_names.erase(key);
  }
  return g_close(key);
}

template <typename T>
void Hook(uintptr_t slot, T* fn, T** prev) {
  *prev = reinterpret_cast<T*>(patch::HookIat(slot, reinterpret_cast<void*>(fn)));
}

}  // namespace

void ApplyRegistryTrace() {
  Hook(kIatRegQueryValueExA, &TQuery, &g_query);
  Hook(kIatRegEnumKeyA, &TEnum, &g_enum);
  Hook(kIatRegOpenKeyExA, &TOpen, &g_open);
  Hook(kIatRegCreateKeyExA, &TCreate, &g_create);
  Hook(kIatRegSetValueExA, &TSet, &g_set);
  Hook(kIatRegQueryInfoKeyA, &TInfo, &g_info);
  Hook(kIatRegCloseKey, &TClose, &g_close);
  Hook(kIatRegDeleteKeyA, &TDelete, &g_delete);
  patch::Log("registry trace: on");
}
