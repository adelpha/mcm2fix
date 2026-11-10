// Windows XP support: helpers for the few Windows calls mcm2fix needs that
// XP does not have (or names differently). Every helper uses the modern call
// when the system has it and an XP-era equivalent otherwise.
#pragma once
#include <windows.h>

#include <cstddef>
#include <string>

namespace xp {

// Logs the Windows version and which XP fallbacks are in use (once).
void LogSystemInfo();

// The per-user local application data folder (no trailing slash), e.g.
// C:\Users\<name>\AppData\Local on Vista+ or
// C:\Documents and Settings\<name>\Local Settings\Application Data on XP.
// Empty if neither %LOCALAPPDATA% nor the shell knows it.
std::string LocalAppData();
std::wstring LocalAppDataW();

// RegCopyTreeA / RegDeleteTreeA (Vista+) with shlwapi fallbacks (XP).
LONG CopyRegTree(HKEY src, HKEY dst);
LONG DeleteRegTree(HKEY parent, const char* subkey);

// A waitable timer: high resolution where Windows supports it.
HANDLE CreateFrameTimer();

// strncat_s(dst, src, _TRUNCATE) without needing the Vista msvcrt.
void Append(char* dst, size_t cap, const char* src);
template <size_t N>
void Append(char (&dst)[N], const char* src) { Append(dst, N, src); }

// strncpy_s(dst, src, _TRUNCATE) likewise.
void Copy(char* dst, size_t cap, const char* src);
template <size_t N>
void Copy(char (&dst)[N], const char* src) { Copy(dst, N, src); }

}  // namespace xp
