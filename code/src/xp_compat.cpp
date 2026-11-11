#include "xp_compat.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <cstring>

#include "patch.h"

namespace xp {

namespace {

template <typename Fn>
Fn Kernel32(const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), name)));
}

template <typename Fn>
Fn Advapi32(const char* name) {
  HMODULE m = GetModuleHandleA("advapi32.dll");
  if (!m) m = LoadLibraryA("advapi32.dll");
  return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

// --- GetTickCount64 for the static winpthreads runtime -------------------
// libwinpthread imports GetTickCount64 (Vista+), which would stop the DLL
// from loading on XP. Defining its import slot here makes the linker use this
// shim instead of kernel32's: the real function where it exists, otherwise
// GetTickCount extended to 64 bits.
ULONGLONG WINAPI TickCount64Fallback() {
  static volatile LONG lock = 0;
  static DWORD last = 0;
  static ULONGLONG high = 0;
  while (InterlockedExchange(&lock, 1)) Sleep(0);
  DWORD now = GetTickCount();
  if (now < last) high += 0x100000000ULL;
  last = now;
  ULONGLONG r = high | now;
  InterlockedExchange(&lock, 0);
  return r;
}

// No function-local static here: its thread-safe initialisation goes through
// winpthreads, which may itself be what is calling.
using TickFn = ULONGLONG(WINAPI*)();
TickFn volatile g_tick64 = nullptr;

ULONGLONG WINAPI TickCount64() {
  TickFn fn = g_tick64;
  if (!fn) {
    fn = Kernel32<TickFn>("GetTickCount64");
    if (!fn) fn = &TickCount64Fallback;
    g_tick64 = fn;
  }
  return fn();
}

}  // namespace
}  // namespace xp

#if defined(__MINGW32__)
extern "C" {
// Import slot looked up by libwinpthread (stdcall decoration: @0).
void* xp_imp_GetTickCount64 __asm__("__imp__GetTickCount64@0") =
    reinterpret_cast<void*>(&xp::TickCount64);
}
#endif

namespace xp {

void Copy(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  dst[0] = '\0';
  Append(dst, cap, src);
}

void Append(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  size_t len = 0;
  while (len < cap && dst[len]) ++len;
  if (len >= cap) {
    dst[cap - 1] = '\0';
    return;
  }
  size_t room = cap - 1 - len;
  size_t n = std::strlen(src);
  if (n > room) n = room;
  std::memcpy(dst + len, src, n);
  dst[len + n] = '\0';
}

namespace {

const char* g_appdata_source = "none";

std::string FindLocalAppData() {
  char buf[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    g_appdata_source = "%LOCALAPPDATA%";
    return buf;
  }
  // XP has no %LOCALAPPDATA%; ask the shell (shell32 5.0+).
  buf[0] = '\0';
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr,
                                 SHGFP_TYPE_CURRENT, buf)) && buf[0]) {
    g_appdata_source = "SHGetFolderPath(CSIDL_LOCAL_APPDATA)";
    return buf;
  }
  return "";
}

}  // namespace

std::string LocalAppData() {
  static const std::string dir = FindLocalAppData();
  return dir;
}

std::wstring LocalAppDataW() {
  static const std::wstring dir = [] {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf);
    buf[0] = L'\0';
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr,
                                   SHGFP_TYPE_CURRENT, buf)) && buf[0])
      return std::wstring(buf);
    return std::wstring();
  }();
  return dir;
}

LONG CopyRegTree(HKEY src, HKEY dst) {
  using Fn = LONG(WINAPI*)(HKEY, LPCSTR, HKEY);
  static Fn fn = Advapi32<Fn>("RegCopyTreeA");
  if (fn) return fn(src, nullptr, dst);
  LONG r = static_cast<LONG>(SHCopyKeyA(src, nullptr, dst, 0));
  patch::Log("xp: RegCopyTreeA missing, SHCopyKeyA -> %ld", r);
  return r;
}

LONG DeleteRegTree(HKEY parent, const char* subkey) {
  using Fn = LONG(WINAPI*)(HKEY, LPCSTR);
  static Fn fn = Advapi32<Fn>("RegDeleteTreeA");
  if (fn) return fn(parent, subkey);
  LONG r = static_cast<LONG>(SHDeleteKeyA(parent, subkey));
  patch::Log("xp: RegDeleteTreeA missing, SHDeleteKeyA(%s) -> %ld", subkey, r);
  return r;
}

HANDLE CreateFrameTimer() {
  using Fn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
  static Fn fn = Kernel32<Fn>("CreateWaitableTimerExW");
  if (fn) {
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+), else a normal one.
    if (HANDLE t = fn(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS)) return t;
    if (HANDLE t = fn(nullptr, nullptr, 0, TIMER_ALL_ACCESS)) return t;
  }
  HANDLE t = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  patch::Log("xp: frame timer via CreateWaitableTimerW (%s)", t ? "ok" : "failed");
  return t;
}

void LogSystemInfo() {
  OSVERSIONINFOEXA v{};
  v.dwOSVersionInfoSize = sizeof(v);
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  GetVersionExA(reinterpret_cast<OSVERSIONINFOA*>(&v));
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
  // GetVersionEx reports 6.2 on Windows 8+ for unmanifested programs; the
  // build number from ntdll is accurate.
  using RtlFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  OSVERSIONINFOEXW rv{};
  rv.dwOSVersionInfoSize = sizeof(rv);
  auto rtl = reinterpret_cast<RtlFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")));
  if (rtl && rtl(&rv) == 0) {
    v.dwMajorVersion = rv.dwMajorVersion;
    v.dwMinorVersion = rv.dwMinorVersion;
    v.dwBuildNumber = rv.dwBuildNumber;
  }
  patch::Log("system: Windows %lu.%lu build %lu %s (SP %u.%u)", v.dwMajorVersion, v.dwMinorVersion,
             v.dwBuildNumber, v.szCSDVersion, v.wServicePackMajor, v.wServicePackMinor);
  patch::Log("system: local app data from %s: %s", g_appdata_source, LocalAppData().c_str());
  patch::Log("system: GetTickCount64 %s, RegCopyTreeA %s, CreateWaitableTimerExW %s, d3d11.dll %s, "
             "d3d9.dll %s, d3dcompiler_47.dll %s, WIC %s",
             Kernel32<void*>("GetTickCount64") ? "yes" : "no (XP fallback)",
             Advapi32<void*>("RegCopyTreeA") ? "yes" : "no (XP fallback)",
             Kernel32<void*>("CreateWaitableTimerExW") ? "yes" : "no (XP fallback)",
             SearchPathA(nullptr, "d3d11.dll", nullptr, 0, nullptr, nullptr) ? "present" : "missing",
             SearchPathA(nullptr, "d3d9.dll", nullptr, 0, nullptr, nullptr) ? "present" : "missing",
             SearchPathA(nullptr, "d3dcompiler_47.dll", nullptr, 0, nullptr, nullptr) ? "present" : "missing",
             SearchPathA(nullptr, "windowscodecs.dll", nullptr, 0, nullptr, nullptr) ? "present" : "missing");
}

}  // namespace xp
