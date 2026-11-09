// In-game Help: open an HTML copy of MCM2HELP.HLP instead of the .HLP.
//
// The Help buttons call 0x529380, which finds <game>\MCM2HELP.hlp, minimizes
// the game window and hands the file to ShellExecuteA. Windows 10 and 11 no
// longer ship WinHelp (winhlp32.exe), so that opens a web page saying .HLP is
// unsupported. tools/hlp2html.py converts the help file to MCM2HELP.html; this
// hook swaps the .hlp for that file (same folder, same name) when it exists.
#include <windows.h>
#include <shellapi.h>

#include <cstring>
#include <string>

#include "patch.h"

namespace {

constexpr uintptr_t kIatShellExecuteA = 0x5552B4;

using ShellExecuteAFn = HINSTANCE(WINAPI*)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
ShellExecuteAFn g_shell_execute = nullptr;

bool EndsWithNoCase(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool FileExists(const std::string& path) {
  DWORD a = GetFileAttributesA(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

HINSTANCE WINAPI HookedShellExecuteA(HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR params,
                                     LPCSTR dir, INT show) {
  patch::Log("help: ShellExecuteA(op=%s, file=%s, params=%s, dir=%s, show=%d)",
             op ? op : "(null)", file ? file : "(null)", params ? params : "(null)",
             dir ? dir : "(null)", show);
  if (file && EndsWithNoCase(file, ".hlp")) {
    std::string html(file);
    html.resize(html.size() - 4);
    html += ".html";
    // A relative name is relative to the dir argument.
    std::string full = html;
    if (dir && *dir && !(html.size() > 1 && (html[1] == ':' || html[0] == '\\'))) {
      full = std::string(dir) + "\\" + html;
    }
    if (FileExists(full)) {
      HINSTANCE r = g_shell_execute(hwnd, nullptr, full.c_str(), nullptr, dir, SW_SHOWNORMAL);
      patch::Log("help: opened %s -> %d", full.c_str(),
                 static_cast<int>(reinterpret_cast<INT_PTR>(r)));
      if (reinterpret_cast<INT_PTR>(r) > 32) return r;
      patch::Log("help: opening the HTML help failed (error %lu); trying the .hlp",
                 GetLastError());
    } else {
      patch::Log("help: %s not found; opening the .hlp as before", full.c_str());
    }
  }
  HINSTANCE r = g_shell_execute(hwnd, op, file, params, dir, show);
  patch::Log("help: ShellExecuteA -> %d", static_cast<int>(reinterpret_cast<INT_PTR>(r)));
  return r;
}

}  // namespace

void ApplyHelpFix() {
  g_shell_execute = reinterpret_cast<ShellExecuteAFn>(
      patch::HookIat(kIatShellExecuteA, reinterpret_cast<void*>(&HookedShellExecuteA)));
  patch::Log("help: ShellExecuteA hooked (HTML help)");
}
