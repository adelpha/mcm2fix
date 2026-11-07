// Keep the game's file writes out of its install folder.
//
// Mcm2.exe writes replays, ghosts, rider profiles, user tracks, circuits,
// logs and audio .ini files next to itself (see docs/runtime-writes.md).
// That fails for a standard user under Program Files. We hook the exe's
// file imports and overlay a per-user folder on top of the game folder:
//
//   %LOCALAPPDATA%\Motocross Madness 2\<path relative to the game folder>
//
// - Writes always go to the per-user folder (copy-on-write for files opened
//   to modify in place).
// - Reads use the per-user copy if it exists, else the game folder.
// - Directory listings merge both folders, per-user entries first.
// Paths outside the game folder are passed through untouched.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "patch.h"

namespace {

constexpr uintptr_t kIatCreateFileA = 0x5551EC;
constexpr uintptr_t kIatDeleteFileA = 0x555244;
constexpr uintptr_t kIatCreateDirectoryA = 0x555278;
constexpr uintptr_t kIatRemoveDirectoryA = 0x555240;
constexpr uintptr_t kIatCopyFileA = 0x55528C;
constexpr uintptr_t kIatFindFirstFileA = 0x55524C;
constexpr uintptr_t kIatFindNextFileA = 0x555248;
constexpr uintptr_t kIatFindClose = 0x555250;
constexpr uintptr_t kIatWritePrivateProfileStringA = 0x5551B8;
constexpr uintptr_t kIatGetPrivateProfileStringA = 0x555204;
constexpr uintptr_t kIatGetPrivateProfileIntA = 0x555208;

std::string g_game;  // game folder, no trailing slash
std::string g_user;  // per-user folder, no trailing slash

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool Exists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Returns the path relative to the game folder ("" for the folder itself),
// or false if the path is outside it.
bool RelativeToGame(LPCSTR path, std::string* rel) {
  if (path == nullptr || *path == '\0') return false;
  char full[MAX_PATH * 2];
  DWORD n = GetFullPathNameA(path, sizeof(full), full, nullptr);
  if (n == 0 || n >= sizeof(full)) return false;
  std::string f(full);
  const std::string lf = Lower(f), lg = Lower(g_game);
  if (lf == lg) {
    rel->clear();
    return true;
  }
  if (lf.size() > lg.size() && lf.compare(0, lg.size(), lg) == 0 && lf[lg.size()] == '\\') {
    *rel = f.substr(lg.size() + 1);
    return true;
  }
  return false;
}

std::string UserPath(const std::string& rel) { return rel.empty() ? g_user : g_user + "\\" + rel; }
std::string GamePath(const std::string& rel) { return rel.empty() ? g_game : g_game + "\\" + rel; }

void MakeDirs(const std::string& dir) {
  if (dir.empty() || Exists(dir)) return;
  size_t slash = dir.find_last_of('\\');
  if (slash != std::string::npos) MakeDirs(dir.substr(0, slash));
  CreateDirectoryA(dir.c_str(), nullptr);
}

void MakeParentDirs(const std::string& file) {
  size_t slash = file.find_last_of('\\');
  if (slash != std::string::npos) MakeDirs(file.substr(0, slash));
}

// Where to read rel from.
std::string ReadPath(const std::string& rel) {
  std::string user = UserPath(rel);
  return Exists(user) ? user : GamePath(rel);
}

// Where to write rel to. If copy_existing, seed the per-user copy from the
// game folder so an in-place modification keeps the original contents.
std::string WritePath(const std::string& rel, bool copy_existing) {
  std::string user = UserPath(rel);
  MakeParentDirs(user);
  if (copy_existing && !Exists(user)) {
    std::string game = GamePath(rel);
    if (Exists(game)) CopyFileA(game.c_str(), user.c_str(), TRUE);
  }
  return user;
}

// ---- CreateFileA -----------------------------------------------------------

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
CreateFileAFn g_create_file = nullptr;

HANDLE WINAPI HookedCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                DWORD disposition, DWORD flags, HANDLE templ) {
  std::string rel;
  if (!RelativeToGame(name, &rel))
    return g_create_file(name, access, share, sa, disposition, flags, templ);
  const bool writes = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
                                 FILE_APPEND_DATA | DELETE)) != 0 ||
                      disposition == CREATE_NEW || disposition == CREATE_ALWAYS ||
                      disposition == TRUNCATE_EXISTING;
  std::string target;
  if (!writes) {
    target = ReadPath(rel);
  } else {
    // Keep existing contents when the file is opened to modify in place.
    const bool keep = disposition == OPEN_EXISTING || disposition == OPEN_ALWAYS;
    if (disposition == CREATE_NEW && Exists(GamePath(rel)) && !Exists(UserPath(rel))) {
      SetLastError(ERROR_FILE_EXISTS);
      return INVALID_HANDLE_VALUE;
    }
    target = WritePath(rel, keep);
  }
  return g_create_file(target.c_str(), access, share, sa, disposition, flags, templ);
}

// ---- Delete / directories / copy ---------------------------------------------

using DeleteFileAFn = BOOL(WINAPI*)(LPCSTR);
using CreateDirectoryAFn = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using RemoveDirectoryAFn = BOOL(WINAPI*)(LPCSTR);
using CopyFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);
DeleteFileAFn g_delete_file = nullptr;
CreateDirectoryAFn g_create_dir = nullptr;
RemoveDirectoryAFn g_remove_dir = nullptr;
CopyFileAFn g_copy_file = nullptr;

BOOL WINAPI HookedDeleteFileA(LPCSTR name) {
  std::string rel;
  if (!RelativeToGame(name, &rel)) return g_delete_file(name);
  BOOL user_ok = g_delete_file(UserPath(rel).c_str());
  // Also try the game folder copy; this only works where it is writable.
  BOOL game_ok = Exists(GamePath(rel)) ? g_delete_file(GamePath(rel).c_str()) : FALSE;
  if (user_ok || game_ok) return TRUE;
  SetLastError(ERROR_FILE_NOT_FOUND);
  return FALSE;
}

BOOL WINAPI HookedCreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa) {
  std::string rel;
  if (!RelativeToGame(name, &rel)) return g_create_dir(name, sa);
  if (Exists(GamePath(rel)) || Exists(UserPath(rel))) {
    SetLastError(ERROR_ALREADY_EXISTS);
    return FALSE;
  }
  MakeParentDirs(UserPath(rel));
  return g_create_dir(UserPath(rel).c_str(), sa);
}

BOOL WINAPI HookedRemoveDirectoryA(LPCSTR name) {
  std::string rel;
  if (!RelativeToGame(name, &rel)) return g_remove_dir(name);
  BOOL user_ok = g_remove_dir(UserPath(rel).c_str());
  BOOL game_ok = Exists(GamePath(rel)) ? g_remove_dir(GamePath(rel).c_str()) : FALSE;
  return user_ok || game_ok;
}

BOOL WINAPI HookedCopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
  std::string rel_from, rel_to, src = from ? from : "", dst = to ? to : "";
  if (RelativeToGame(from, &rel_from)) src = ReadPath(rel_from);
  if (RelativeToGame(to, &rel_to)) {
    if (fail_if_exists && Exists(GamePath(rel_to)) && !Exists(UserPath(rel_to))) {
      SetLastError(ERROR_FILE_EXISTS);
      return FALSE;
    }
    dst = WritePath(rel_to, false);
  }
  return g_copy_file(src.c_str(), dst.c_str(), fail_if_exists);
}

// ---- Merged directory listings -------------------------------------------------

using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using FindNextFileAFn = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
using FindCloseFn = BOOL(WINAPI*)(HANDLE);
FindFirstFileAFn g_find_first = nullptr;
FindNextFileAFn g_find_next = nullptr;
FindCloseFn g_find_close = nullptr;

struct MergedFind {
  std::vector<WIN32_FIND_DATAA> entries;
  size_t next = 0;
};
std::unordered_set<MergedFind*> g_finds;
CRITICAL_SECTION g_finds_lock;

void Collect(const std::string& pattern, std::vector<WIN32_FIND_DATAA>* out,
             std::unordered_set<std::string>* seen) {
  WIN32_FIND_DATAA fd;
  HANDLE h = g_find_first(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (seen->insert(Lower(fd.cFileName)).second) out->push_back(fd);
  } while (g_find_next(h, &fd));
  g_find_close(h);
}

HANDLE WINAPI HookedFindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
  std::string rel;
  if (!RelativeToGame(pattern, &rel) || rel.empty()) return g_find_first(pattern, data);
  auto* merged = new MergedFind;
  std::unordered_set<std::string> seen;
  Collect(UserPath(rel), &merged->entries, &seen);
  Collect(GamePath(rel), &merged->entries, &seen);
  if (merged->entries.empty()) {
    delete merged;
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  *data = merged->entries[0];
  merged->next = 1;
  EnterCriticalSection(&g_finds_lock);
  g_finds.insert(merged);
  LeaveCriticalSection(&g_finds_lock);
  return reinterpret_cast<HANDLE>(merged);
}

MergedFind* AsMerged(HANDLE h) {
  auto* m = reinterpret_cast<MergedFind*>(h);
  EnterCriticalSection(&g_finds_lock);
  bool ours = g_finds.count(m) != 0;
  LeaveCriticalSection(&g_finds_lock);
  return ours ? m : nullptr;
}

BOOL WINAPI HookedFindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data) {
  MergedFind* m = AsMerged(h);
  if (!m) return g_find_next(h, data);
  if (m->next >= m->entries.size()) {
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
  }
  *data = m->entries[m->next++];
  return TRUE;
}

BOOL WINAPI HookedFindClose(HANDLE h) {
  MergedFind* m = AsMerged(h);
  if (!m) return g_find_close(h);
  EnterCriticalSection(&g_finds_lock);
  g_finds.erase(m);
  LeaveCriticalSection(&g_finds_lock);
  delete m;
  return TRUE;
}

// ---- Private profile (.ini) files ------------------------------------------------

using WritePrivateProfileStringAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPCSTR);
using GetPrivateProfileStringAFn = DWORD(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
using GetPrivateProfileIntAFn = UINT(WINAPI*)(LPCSTR, LPCSTR, INT, LPCSTR);
WritePrivateProfileStringAFn g_write_profile = nullptr;
GetPrivateProfileStringAFn g_get_profile_string = nullptr;
GetPrivateProfileIntAFn g_get_profile_int = nullptr;

// Profile APIs treat a bare file name as relative to the Windows folder, so
// only remap names that contain a path separator.
bool ProfileRel(LPCSTR file, std::string* rel) {
  if (file == nullptr || std::strpbrk(file, "\\/") == nullptr) return false;
  return RelativeToGame(file, rel);
}

BOOL WINAPI HookedWritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value,
                                             LPCSTR file) {
  std::string rel;
  if (!ProfileRel(file, &rel)) return g_write_profile(section, key, value, file);
  return g_write_profile(section, key, value, WritePath(rel, true).c_str());
}

DWORD WINAPI HookedGetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out,
                                            DWORD size, LPCSTR file) {
  std::string rel;
  if (!ProfileRel(file, &rel)) return g_get_profile_string(section, key, def, out, size, file);
  return g_get_profile_string(section, key, def, out, size, ReadPath(rel).c_str());
}

UINT WINAPI HookedGetPrivateProfileIntA(LPCSTR section, LPCSTR key, INT def, LPCSTR file) {
  std::string rel;
  if (!ProfileRel(file, &rel)) return g_get_profile_int(section, key, def, file);
  return g_get_profile_int(section, key, def, ReadPath(rel).c_str());
}

template <typename Fn>
void Hook(uintptr_t slot, Fn replacement, Fn* original) {
  *original = reinterpret_cast<Fn>(patch::HookIat(slot, reinterpret_cast<void*>(replacement)));
}

}  // namespace

void ApplyFileFixes() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (char* slash = std::strrchr(path, '\\')) *slash = '\0';
  g_game = path;

  // Make relative paths work from any shortcut's "Start in" folder.
  SetCurrentDirectoryA(g_game.c_str());

  char local[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    patch::Log("files: LOCALAPPDATA not set, writes stay in the game folder");
    return;
  }
  g_user = std::string(local) + "\\Motocross Madness 2";
  MakeDirs(g_user);
  InitializeCriticalSection(&g_finds_lock);

  Hook(kIatCreateFileA, &HookedCreateFileA, &g_create_file);
  Hook(kIatDeleteFileA, &HookedDeleteFileA, &g_delete_file);
  Hook(kIatCreateDirectoryA, &HookedCreateDirectoryA, &g_create_dir);
  Hook(kIatRemoveDirectoryA, &HookedRemoveDirectoryA, &g_remove_dir);
  Hook(kIatCopyFileA, &HookedCopyFileA, &g_copy_file);
  Hook(kIatFindFirstFileA, &HookedFindFirstFileA, &g_find_first);
  Hook(kIatFindNextFileA, &HookedFindNextFileA, &g_find_next);
  Hook(kIatFindClose, &HookedFindClose, &g_find_close);
  Hook(kIatWritePrivateProfileStringA, &HookedWritePrivateProfileStringA, &g_write_profile);
  Hook(kIatGetPrivateProfileStringA, &HookedGetPrivateProfileStringA, &g_get_profile_string);
  Hook(kIatGetPrivateProfileIntA, &HookedGetPrivateProfileIntA, &g_get_profile_int);
  patch::Log("files: game folder %s, user folder %s", g_game.c_str(), g_user.c_str());
}
