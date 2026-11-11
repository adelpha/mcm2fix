#include "patch.h"
#include "xp_compat.h"

#include "config.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace patch {

bool Matches(uintptr_t addr, const void* expected, size_t len) {
  return std::memcmp(reinterpret_cast<const void*>(addr), expected, len) == 0;
}

void Write(uintptr_t addr, const void* data, size_t len) {
  void* p = reinterpret_cast<void*>(addr);
  DWORD old = 0;
  VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
  std::memcpy(p, data, len);
  VirtualProtect(p, len, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, len);
}

static void Branch(uint8_t opcode, uintptr_t addr, uintptr_t target, size_t total_len) {
  uint8_t buf[32];
  if (total_len < 5 || total_len > sizeof(buf)) return;
  std::memset(buf, 0x90, sizeof(buf));
  buf[0] = opcode;
  int32_t rel = static_cast<int32_t>(target - (addr + 5));
  std::memcpy(buf + 1, &rel, 4);
  Write(addr, buf, total_len);
}

void Call(uintptr_t addr, const void* target, size_t total_len) {
  Branch(0xE8, addr, reinterpret_cast<uintptr_t>(target), total_len);
}

void Jump(uintptr_t addr, uintptr_t target, size_t total_len) {
  Branch(0xE9, addr, target, total_len);
}

bool Replace(const char* name, uintptr_t addr, const void* original, size_t len,
             const void* replacement, size_t replacement_len) {
  if (!Matches(addr, original, len)) {
    Log("patch %s: unexpected bytes at %08x, skipped", name, static_cast<unsigned>(addr));
    return false;
  }
  uint8_t buf[64];
  if (len > sizeof(buf) || replacement_len > len) return false;
  std::memset(buf, 0x90, len);
  std::memcpy(buf, replacement, replacement_len);
  Write(addr, buf, len);
  Log("patch %s: applied at %08x", name, static_cast<unsigned>(addr));
  return true;
}

void* HookIat(uintptr_t slot, void* replacement) {
  void* previous = *reinterpret_cast<void**>(slot);
  Write(slot, &replacement, sizeof(replacement));
  return previous;
}

namespace {
constexpr size_t kKeepLogs = 50;
// Deletes the oldest mcm2fix-*.log files in dir so that at most
// kKeepLogs - 1 remain (the new one makes kKeepLogs). Names sort by time.
void PruneLogs(const char* dir) {
  std::vector<std::string> names;
  WIN32_FIND_DATAA fd{};
  std::string pattern = std::string(dir) + "\\mcm2fix-*.log";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  std::sort(names.begin(), names.end());
  for (size_t i = 0; i + kKeepLogs - 1 < names.size(); ++i)
    DeleteFileA((std::string(dir) + "\\" + names[i]).c_str());
}
}  // namespace

void Log(const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > static_cast<int>(sizeof(msg)) - 2) n = sizeof(msg) - 2;
  msg[n] = '\n';
  msg[n + 1] = 0;
  OutputDebugStringA("[mcm2fix] ");
  OutputDebugStringA(msg);
  if (!GetConfig().log) return;
  static HANDLE file = INVALID_HANDLE_VALUE;
  static bool tried = false;
  if (!tried) {
    tried = true;
    char path[MAX_PATH] = {};
    const std::string local = xp::LocalAppData();
    if (!local.empty() && local.size() < MAX_PATH - 60) {
      xp::Append(path, local.c_str());
      xp::Append(path, "\\Motocross Madness 2");
      CreateDirectoryA(path, nullptr);
      // One file per run: logs\mcm2fix-YYYYMMDD-HHMMSS.log, keeping the
      // newest kKeepLogs.
      xp::Append(path, "\\logs");
      CreateDirectoryA(path, nullptr);
      PruneLogs(path);
      SYSTEMTIME st;
      GetLocalTime(&st);
      char name[64];
      std::snprintf(name, sizeof(name), "\\mcm2fix-%04u%02u%02u-%02u%02u%02u.log", st.wYear, st.wMonth,
                    st.wDay, st.wHour, st.wMinute, st.wSecond);
      xp::Append(path, name);
      file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    }
  }
  if (file != INVALID_HANDLE_VALUE) {
    char line[600];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int m = std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%5lu] %s", t.wHour,
                          t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    DWORD written = 0;
    if (m > 0) WriteFile(file, line, static_cast<DWORD>(m), &written, nullptr);
  }
}

}  // namespace patch
