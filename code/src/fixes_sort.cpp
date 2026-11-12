// Race hitches from the game's sorting (found with Profile=1 on a custom map
// with many trees, 2026-11-12: every in-race frame over 20 ms was inside the
// CRT qsort).
//
//  - 0x539a96 is the CRT's qsort (33 call sites). Its quicksort partition
//    goes quadratic when many elements compare equal.
//  - The vegetation renderer (cull/collect 0x45b890) keeps a cache of built
//    tree data (object at +0x59c; +0 frame number, +4 count, +0xc entries
//    {last-used frame, free fn, args, size}, +0x10 bytes). Each frame, when
//    more than 256 KB of it went unused that frame (0x45b8ef), it trims
//    (0x4011b0): qsort of ALL entries by last-used frame, then frees unused
//    ones. Most entries share the current frame number, so on a tree-heavy
//    map that sort took 35-125 ms, mostly while turning the camera (trees
//    leave the view, their data goes stale) - and the freed trees are then
//    built again when they come back into view.
//
// FastSort=1 replaces qsort with a stable merge sort (O(n log n) whatever the
// keys; same comparator, so the same order apart from equal elements, which
// qsort leaves in no particular order anyway). TreeCacheMB raises the 256 KB
// threshold so the cache is trimmed far less often.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "config.h"
#include "patch.h"

namespace {

using CompareFn = int(__cdecl*)(const void*, const void*);

LARGE_INTEGER g_freq;
bool g_log = false;
int g_slow_left = 200;
DWORD g_last_summary = 0;

struct CallerStats {
  unsigned calls = 0;
  size_t max_n = 0;
  double total_ms = 0.0, max_ms = 0.0;
};
std::map<uintptr_t, CallerStats> g_stats;
CRITICAL_SECTION g_cs;  // stats and log counters (a sort may come from any thread)

double Ms(LONGLONG d) { return static_cast<double>(d) * 1000.0 / g_freq.QuadPart; }

void Summary() {
  const DWORD now = GetTickCount();
  if (!g_last_summary) g_last_summary = now;
  if (now - g_last_summary < 10000) return;
  g_last_summary = now;
  if (g_stats.empty()) return;
  for (const auto& kv : g_stats)
    patch::Log("sort: 10 s: caller %06x calls=%u max n=%u total %.2f ms max %.2f ms", static_cast<unsigned>(kv.first),
               kv.second.calls, static_cast<unsigned>(kv.second.max_n), kv.second.total_ms, kv.second.max_ms);
  g_stats.clear();
}

void Sort(void* base, size_t n, size_t width, CompareFn cmp) {
  auto* p = static_cast<uint8_t*>(base);
  // Small arrays (the scene walk at 0x4e1efb sorts ~35,000 of up to ~30
  // elements a second): stable insertion sort in place, no allocation.
  uint8_t tmp[256];
  if (n <= 32 && width <= sizeof(tmp)) {
    for (size_t i = 1; i < n; ++i) {
      size_t j = i;
      while (j > 0 && cmp(p + (j - 1) * width, p + i * width) > 0) --j;
      if (j == i) continue;
      std::memcpy(tmp, p + i * width, width);
      std::memmove(p + (j + 1) * width, p + j * width, (i - j) * width);
      std::memcpy(p + j * width, tmp, width);
    }
    return;
  }
  const size_t bytes = n * width;
  std::vector<uint8_t> copy(p, p + bytes);
  std::vector<uint32_t> index(n);
  for (size_t i = 0; i < n; ++i) index[i] = static_cast<uint32_t>(i);
  const uint8_t* src = copy.data();
  std::stable_sort(index.begin(), index.end(), [&](uint32_t a, uint32_t b) {
    return cmp(src + a * width, src + b * width) < 0;
  });
  for (size_t i = 0; i < n; ++i) std::memcpy(p + i * width, src + index[i] * width, width);
}

void __cdecl FastQsort(void* base, size_t n, size_t width, CompareFn cmp) {
  const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if (n < 2 || width == 0 || !base || !cmp) return;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  Sort(base, n, width, cmp);
  QueryPerformanceCounter(&t1);
  const double ms = Ms(t1.QuadPart - t0.QuadPart);
  if (ms <= 2.0 && !g_log) return;
  EnterCriticalSection(&g_cs);
  if (ms > 2.0 && g_slow_left > 0) {
    --g_slow_left;
    patch::Log("sort: slow sort from %06x: %u elements of %u bytes took %.2f ms", static_cast<unsigned>(caller),
               static_cast<unsigned>(n), static_cast<unsigned>(width), ms);
  }
  if (g_log) {
    CallerStats& s = g_stats[caller];
    ++s.calls;
    s.max_n = std::max(s.max_n, n);
    s.total_ms += ms;
    s.max_ms = std::max(s.max_ms, ms);
    Summary();
  }
  LeaveCriticalSection(&g_cs);
}

void PatchQsort() {
  const uint8_t orig[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xF8, 0x00, 0x00, 0x00};  // push ebp; mov ebp,esp; sub esp,0F8h
  if (!patch::Matches(0x539A96, orig, sizeof(orig))) {
    patch::Log("sort: qsort code differs, FastSort not applied");
    return;
  }
  patch::Jump(0x539A96, reinterpret_cast<uintptr_t>(&FastQsort), sizeof(orig));
  patch::Log("sort: FastSort=1, CRT qsort replaced with a stable merge sort (SortLog=%d)", g_log ? 1 : 0);
}

void PatchTreeCache(int mb) {
  const uint8_t orig[] = {0x81, 0xF9, 0x00, 0x00, 0x04, 0x00};  // cmp ecx,40000h
  if (!patch::Matches(0x45B8EF, orig, sizeof(orig))) {
    patch::Log("sort: tree cache threshold code differs, TreeCacheMB not applied");
    return;
  }
  const uint32_t bytes = static_cast<uint32_t>(mb) * 1024u * 1024u;
  uint8_t code[6] = {0x81, 0xF9};
  std::memcpy(code + 2, &bytes, 4);
  patch::Write(0x45B8EF, code, sizeof(code));
  patch::Log("sort: tree cache trimmed only when over %d MB of it is unused (was 256 KB)", mb);
}

}  // namespace

void ApplySortFixes() {
  QueryPerformanceFrequency(&g_freq);
  InitializeCriticalSection(&g_cs);
  const Config& c = GetConfig();
  g_log = c.sort_log;
  if (c.fast_sort) PatchQsort();
  else patch::Log("sort: FastSort=0, CRT qsort left as it is");
  if (c.tree_cache_mb > 0) PatchTreeCache(c.tree_cache_mb);
  else patch::Log("sort: TreeCacheMB=0, tree cache trimmed at 256 KB as in the original");
}
