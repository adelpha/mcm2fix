// Performance profiling (ini Profile=1).
//
// What it writes, next to the run's log in %LOCALAPPDATA%\Motocross Madness 2\logs:
// - in the log, every 5 seconds: frame-time statistics (average, percentiles,
//   hitches), how long each timed section took per frame, and per-frame
//   renderer counts (draws, state changes, uploads, read-backs, ...);
//   and for each hitch (a frame much longer than the ones around it) what
//   that frame spent its time on;
// - perf-<date>-<time>.csv: one line per displayed frame with the same data;
// - perf-samples-<date>-<time>.txt: a sampling profile of the game thread
//   (ProfileSampleUs, default every 1000 us): where it was (game code, our
//   DLL, Windows, the graphics driver), which game code called into the
//   DLLs, and how often each game call site was on the stack (inclusive
//   time). Rewritten every 15 seconds with the totals so far.
#include "prof.h"

#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "framerate.h"
#include "patch.h"

namespace prof {
namespace {

const char* const kZoneNames[kZoneCount] = {
    "tick",      "sim",       "sim.control", "sim.manager", "sim.state", "draw",      "smooth.begin",
    "smooth.end", "present",  "postframe",   "flip",        "capwait",   "hudflush",  "compose",
    "swappresent", "submit",  "vertexprep",  "stateapply", "drawcall",  "smoothverts", "upload",  "readback",
    "keyed",     "blt",       "lock",        "getdc",       "clear",     "gditext",
};
const char* const kCounterNames[cCounterCount] = {
    "steps", "draws", "draws_tl", "draws_idx", "verts", "indices", "tex_changes", "state_changes",
    "target_changes", "uploads", "upload_kb", "readbacks", "locks", "blts", "getdcs", "clears", "tex_creates",
};

bool g_on = false;
DWORD g_main_tid = 0;
double g_ms_per_tick = 0.0;
LARGE_INTEGER g_freq{};

struct FrameAcc {
  int64_t zt[kZoneCount];
  uint32_t zc[kZoneCount];
  uint32_t c[cCounterCount];
};
FrameAcc g_cur{};
volatile LONG g_zone = -1;      // innermost zone running on the game thread (for the sampler)
volatile LONG g_frame = 0;      // frames shown so far
int64_t g_last_frame_end = 0;
bool g_in_tick = false;
bool g_frame_pending = false;
int64_t g_start = 0;
double g_gpu_last = -1.0;

// Changes between draws.
const void* g_last_tex = nullptr;
const void* g_last_blend = nullptr;
const void* g_last_depth = nullptr;
const void* g_last_raster = nullptr;
const void* g_last_rtv = nullptr;

int64_t Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

double Ms(int64_t ticks) { return static_cast<double>(ticks) * g_ms_per_tick; }

bool OnMain() { return GetCurrentThreadId() == g_main_tid; }

// ---------------------------------------------------------------- 5 s windows

struct Window {
  int64_t start = 0;
  std::vector<float> frame_ms, busy_ms;
  double zt[kZoneCount] = {};
  double zc[kZoneCount] = {};
  double c[cCounterCount] = {};
  unsigned steps_hist[4] = {};
  unsigned hitches = 0;
  unsigned race_frames = 0;
  double gpu_sum = 0, gpu_max = 0;
  unsigned gpu_n = 0;
};
Window g_win;

// Recent frame times for the hitch threshold.
float g_recent[240] = {};
unsigned g_recent_n = 0;
float g_median = 0.0f;
unsigned g_hitch_logs = 0;

// Hitch frames handed to the sampler thread (it lists their samples).
struct Hitch {
  LONG frame;
  float ms;
};
Hitch g_hitch_q[64];
std::atomic<unsigned> g_hitch_w{0};

float Percentile(std::vector<float>& v, double p) {
  if (v.empty()) return 0.0f;
  size_t k = static_cast<size_t>(p * (v.size() - 1) + 0.5);
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

// ---------------------------------------------------------------- files

char g_stamp[32] = {};
char g_dir[MAX_PATH] = {};
CRITICAL_SECTION g_csv_cs;
std::string g_csv_buf;
HANDLE g_csv = INVALID_HANDLE_VALUE;
uint64_t g_csv_bytes = 0;

void OpenFiles() {
  DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", g_dir, MAX_PATH);
  if (!len || len > MAX_PATH - 80) return;
  strncat_s(g_dir, "\\Motocross Madness 2\\logs", _TRUNCATE);
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::snprintf(g_stamp, sizeof(g_stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond);
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\perf-%s.csv", g_dir, g_stamp);
  g_csv = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  std::string head = "frame,t_s,race,frame_ms,busy_ms,gpu_ms_lagged";
  for (int z = 0; z < kZoneCount; ++z) head += std::string(",") + kZoneNames[z] + "_ms";
  for (int z = 0; z < kZoneCount; ++z) head += std::string(",") + kZoneNames[z] + "_n";
  for (int c = 0; c < cCounterCount; ++c) head += std::string(",") + kCounterNames[c];
  head += "\n";
  EnterCriticalSection(&g_csv_cs);
  g_csv_buf += head;
  LeaveCriticalSection(&g_csv_cs);
}

void FlushCsv(bool at_exit = false) {
  std::string out;
  if (at_exit) {
    if (!TryEnterCriticalSection(&g_csv_cs)) return;
  } else {
    EnterCriticalSection(&g_csv_cs);
  }
  out.swap(g_csv_buf);
  LeaveCriticalSection(&g_csv_cs);
  if (out.empty() || g_csv == INVALID_HANDLE_VALUE) return;
  if (g_csv_bytes > 400ull * 1024 * 1024) return;  // enough
  DWORD w = 0;
  WriteFile(g_csv, out.data(), static_cast<DWORD>(out.size()), &w, nullptr);
  g_csv_bytes += out.size();
}

// ---------------------------------------------------------------- sampler

uintptr_t g_exe_lo = 0, g_exe_hi = 0;    // exe code
uintptr_t g_dll_lo = 0, g_dll_hi = 0;    // our DLL's code
uintptr_t g_dll_base = 0;
uintptr_t g_stack_hi = 0;                // game thread's stack base
HANDLE g_thread = nullptr;               // game thread (for Suspend/GetThreadContext)
HANDLE g_sampler = nullptr;
volatile bool g_stop = false;

constexpr int kChain = 6;
struct Sample {
  LONG frame;
  int zone;
  uint32_t eip;      // exact for exe/DLL code, else the module base | 1
  uint32_t exe_ret;  // first game return address on the stack (0 if none)
  uint32_t dll_ret;  // first mcm2fix return address on the stack (0 if none)
};
constexpr unsigned kRing = 1 << 14;
Sample g_ring[kRing];
unsigned g_ring_n = 0;

CRITICAL_SECTION g_map_cs;
using Map = std::unordered_map<uint64_t, uint32_t>;
Map g_self, g_incl, g_exe_call, g_dll_call;
uint32_t g_zone_samples[kZoneCount + 1] = {};
uint32_t g_total_samples = 0;
uint32_t g_failed_samples = 0;

uint64_t Key(int zone, uint32_t addr) { return (static_cast<uint64_t>(zone + 1) << 32) | addr; }

// Code addresses (at least 8 bytes into the section, so AfterCall can look back).
bool InExe(uintptr_t a) { return a >= g_exe_lo + 8 && a < g_exe_hi; }
bool InDll(uintptr_t a) { return a >= g_dll_lo + 8 && a < g_dll_hi; }

// A return address: the bytes before it are a call instruction.
bool AfterCall(uintptr_t a) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
  if (p[-5] == 0xE8) return true;                                   // call rel32
  if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return true;  // call [abs] / [reg+disp32]
  if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true;  // call reg / [reg]
  if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;         // call [reg+disp8]
  if (p[-7] == 0xFF && p[-6] == 0x94) return true;                  // call [sib+disp32]
  if (p[-4] == 0xFF && p[-3] == 0x54) return true;                  // call [sib+disp8]
  return false;
}

uint32_t Normalize(uintptr_t eip) {
  if (InExe(eip) || InDll(eip)) return static_cast<uint32_t>(eip);
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<void*>(eip), &mbi, sizeof(mbi)) && mbi.AllocationBase)
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)) | 1u;
  return 1u;  // unknown
}

std::string Describe(uint32_t a) {
  char buf[MAX_PATH + 32];
  if (a & 1u) {
    uintptr_t base = a & ~1u;
    if (base == 0) return "unknown";
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(reinterpret_cast<HMODULE>(base), path, MAX_PATH)) {
      const char* name = std::strrchr(path, '\\');
      return name ? name + 1 : path;
    }
    std::snprintf(buf, sizeof(buf), "anon@%08x", static_cast<unsigned>(base));
    return buf;
  }
  if (InDll(a)) {
    std::snprintf(buf, sizeof(buf), "mcm2fix+%05x", static_cast<unsigned>(a - g_dll_base));
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "exe:%06x", a);
  return buf;
}

// Every loaded module's executable sections plus its exports, so samples taken
// while the game thread is inside Windows or a driver can name the chain of
// calls that led there (e.g. ntdll!NtWaitForAlertByThreadId < DSOUND+1234 <
// exe:4c1234). Only touched by the sampler thread.
struct ModRange {
  uintptr_t lo, hi, base;
  int mod;
};
struct ModInfo {
  std::string name;
  std::vector<std::pair<uint32_t, std::string>> exports;  // rva -> name, sorted
};
std::vector<ModRange> g_mod_ranges;  // sorted by lo
std::vector<ModInfo> g_mods;
std::map<uintptr_t, int> g_mod_by_base;

void LoadExports(uintptr_t base, ModInfo& mi) {
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!dir.VirtualAddress || !dir.Size) return;
  auto ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  auto names = reinterpret_cast<const DWORD*>(base + ex->AddressOfNames);
  auto ords = reinterpret_cast<const WORD*>(base + ex->AddressOfNameOrdinals);
  auto funcs = reinterpret_cast<const DWORD*>(base + ex->AddressOfFunctions);
  for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
    if (ords[i] >= ex->NumberOfFunctions) continue;
    const DWORD rva = funcs[ords[i]];
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;  // forwarder
    mi.exports.emplace_back(rva, reinterpret_cast<const char*>(base + names[i]));
  }
  std::sort(mi.exports.begin(), mi.exports.end());
}

void RefreshModules() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32 me{};
  me.dwSize = sizeof(me);
  std::vector<ModRange> ranges;
  for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
    int idx;
    auto it = g_mod_by_base.find(base);
    if (it != g_mod_by_base.end() && g_mods[it->second].name == me.szModule) {
      idx = it->second;
    } else {
      ModInfo mi;
      mi.name = me.szModule;
      LoadExports(base, mi);
      idx = static_cast<int>(g_mods.size());
      g_mods.push_back(std::move(mi));
      g_mod_by_base[base] = idx;
    }
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
      const uintptr_t lo = base + sec[i].VirtualAddress;
      ranges.push_back({lo, lo + std::max(sec[i].Misc.VirtualSize, 1ul), base, idx});
    }
  }
  CloseHandle(snap);
  std::sort(ranges.begin(), ranges.end(), [](const ModRange& a, const ModRange& b) { return a.lo < b.lo; });
  g_mod_ranges.swap(ranges);
}

const ModRange* FindCode(uintptr_t a) {
  auto it = std::upper_bound(g_mod_ranges.begin(), g_mod_ranges.end(), a,
                             [](uintptr_t v, const ModRange& r) { return v < r.lo; });
  if (it == g_mod_ranges.begin()) return nullptr;
  --it;
  return a < it->hi ? &*it : nullptr;
}

// "ntdll!RtlEnterCriticalSection+1a", or "DSOUND+1f2a0" when no export is near.
std::string NameAddr(uintptr_t a, const ModRange& r) {
  const ModInfo& mi = g_mods[r.mod];
  const uint32_t rva = static_cast<uint32_t>(a - r.base);
  char buf[160];
  auto it = std::upper_bound(mi.exports.begin(), mi.exports.end(), std::make_pair(rva, std::string("\x7f")));
  if (it != mi.exports.begin()) {
    --it;
    if (rva - it->first < 0x800) {
      std::snprintf(buf, sizeof(buf), "%s!%s+%x", mi.name.c_str(), it->second.c_str(), rva - it->first);
      return buf;
    }
  }
  std::snprintf(buf, sizeof(buf), "%s+%x", mi.name.c_str(), rva);
  return buf;
}

std::unordered_map<std::string, uint32_t> g_via;  // "<zone> <chain>" -> samples

constexpr size_t kStackWords = 16384;  // 64 KB: Windows audio and driver stacks run deep
uint32_t g_stack_copy[kStackWords];

const char* ZoneName(int z) { return z < 0 ? "none" : kZoneNames[z]; }

void TakeSample() {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (SuspendThread(g_thread) == static_cast<DWORD>(-1)) return;
  // While the game thread is stopped: no allocation, no locks, no loader.
  const bool ok = GetThreadContext(g_thread, &ctx) != 0;
  const LONG frame = g_frame;
  const int zone = static_cast<int>(g_zone);
  size_t words = 0;
  if (ok && ctx.Esp && ctx.Esp < g_stack_hi) {
    words = std::min<size_t>((g_stack_hi - ctx.Esp) / 4, kStackWords);
    std::memcpy(g_stack_copy, reinterpret_cast<const void*>(ctx.Esp), words * 4);
  }
  ResumeThread(g_thread);
  if (!ok) {
    ++g_failed_samples;
    return;
  }

  Sample s{frame, zone, Normalize(ctx.Eip), 0, 0};
  uint32_t chain[kChain];
  int nchain = 0;
  for (size_t i = 0; i < words && nchain < kChain; ++i) {
    const uintptr_t v = g_stack_copy[i];
    if (InExe(v) && AfterCall(v)) {
      if (!s.exe_ret) s.exe_ret = static_cast<uint32_t>(v);
      bool dup = false;
      for (int k = 0; k < nchain; ++k) dup |= chain[k] == v;
      if (!dup) chain[nchain++] = static_cast<uint32_t>(v);
    } else if (!s.dll_ret && InDll(v) && AfterCall(v)) {
      s.dll_ret = static_cast<uint32_t>(v);
    }
  }
  g_ring[g_ring_n++ & (kRing - 1)] = s;

  // Outside game code and our DLL: name the chain of calls from the game into it.
  std::string via;
  if (!InExe(ctx.Eip) && !InDll(ctx.Eip)) {
    const ModRange* r = FindCode(ctx.Eip);
    via = r ? NameAddr(ctx.Eip, *r) : "?";
    int parts = 1;
    std::string last_mod = r ? g_mods[r->mod].name : "";
    for (size_t i = 0; i < words && parts < 8; ++i) {
      const uintptr_t v = g_stack_copy[i];
      if (InExe(v) && AfterCall(v)) {
        char b[24];
        std::snprintf(b, sizeof(b), "<exe:%06x", static_cast<unsigned>(v));
        via += b;
        break;
      }
      if (InDll(v) && AfterCall(v)) {
        char b[32];
        std::snprintf(b, sizeof(b), "<mcm2fix+%05x", static_cast<unsigned>(v - g_dll_base));
        via += b;
        ++parts;
        continue;
      }
      const ModRange* m = FindCode(v);
      if (!m || v < m->lo + 8 || !AfterCall(v)) continue;
      // Within one module keep only the outermost-but-first frames: the first
      // return address per module run is enough to see who called whom.
      if (g_mods[m->mod].name == last_mod) continue;
      last_mod = g_mods[m->mod].name;
      via += "<" + NameAddr(v, *m);
      ++parts;
    }
  }

  EnterCriticalSection(&g_map_cs);
  ++g_total_samples;
  ++g_zone_samples[zone + 1];
  ++g_self[Key(zone, s.eip)];
  if (!InExe(ctx.Eip)) {
    if (s.exe_ret) ++g_exe_call[Key(zone, s.exe_ret)];
    if (!InDll(ctx.Eip) && s.dll_ret) ++g_dll_call[Key(zone, s.dll_ret)];
  }
  if (!via.empty()) ++g_via[std::string(ZoneName(zone)) + " " + via];
  for (int k = 0; k < nchain; ++k) ++g_incl[Key(-1, chain[k])];
  if (InExe(ctx.Eip)) ++g_incl[Key(-1, s.eip)];  // the function itself
  LeaveCriticalSection(&g_map_cs);
}


void WriteTop(std::string& out, const char* tag, const Map& m, size_t top) {
  std::vector<std::pair<uint64_t, uint32_t>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  if (v.size() > top) v.resize(top);
  char line[512];
  for (const auto& kv : v) {
    const int zone = static_cast<int>(kv.first >> 32) - 1;
    std::snprintf(line, sizeof(line), "%s %s %s %u\n", tag, ZoneName(zone),
                  Describe(static_cast<uint32_t>(kv.first)).c_str(), kv.second);
    out += line;
  }
}

void DumpSamples() {
  if (!g_dir[0]) return;
  std::string out;
  char line[512];
  {
    if (!TryEnterCriticalSection(&g_map_cs)) return;
    std::snprintf(line, sizeof(line),
                  "# mcm2fix sampling profile: %u samples every %d us (%u failed); dll base %08x code end +%x\n"
                  "# self <zone> <where> <n>: where the game thread was\n"
                  "# execall <zone> <game return address> <n>: game code that called into a DLL (ours, Windows, driver)\n"
                  "# dllcall <zone> <mcm2fix return address> <n>: our code that called into Windows/driver\n"
                  "# incl - <game return address or address> <n>: game call sites on the stack (inclusive)\n"
                  "# via <zone> <where<caller<...<game return address> <n>: call chain into Windows/driver code\n",
                  g_total_samples, GetConfig().profile_sample_us, g_failed_samples, static_cast<unsigned>(g_dll_base),
                  static_cast<unsigned>(g_dll_hi - g_dll_base));
    out += line;
    for (int z = -1; z < kZoneCount; ++z) {
      if (!g_zone_samples[z + 1]) continue;
      std::snprintf(line, sizeof(line), "zone %s %u\n", ZoneName(z), g_zone_samples[z + 1]);
      out += line;
    }
    WriteTop(out, "self", g_self, 1500);
    WriteTop(out, "execall", g_exe_call, 600);
    WriteTop(out, "dllcall", g_dll_call, 400);
    WriteTop(out, "incl", g_incl, 2500);
    {
      std::vector<std::pair<std::string, uint32_t>> v(g_via.begin(), g_via.end());
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      if (v.size() > 800) v.resize(800);
      for (const auto& kv : v) out += "via " + kv.first + " " + std::to_string(kv.second) + "\n";
    }
    LeaveCriticalSection(&g_map_cs);
  }
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\perf-samples-%s.txt", g_dir, g_stamp);
  HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &w, nullptr);
  CloseHandle(f);
}

// List the samples taken during a hitch frame.
void ReportHitch(const Hitch& h) {
  std::unordered_map<uint64_t, unsigned> where;
  std::unordered_map<uint32_t, unsigned> callers;
  unsigned n = 0;
  const unsigned end = g_ring_n, begin = end > kRing ? end - kRing : 0;
  for (unsigned i = begin; i < end; ++i) {
    const Sample& s = g_ring[i & (kRing - 1)];
    if (s.frame != h.frame) continue;
    ++n;
    ++where[Key(s.zone, s.eip)];
    if (s.exe_ret) ++callers[s.exe_ret];
  }
  std::vector<std::pair<uint64_t, unsigned>> v(where.begin(), where.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string text;
  char part[160];
  for (size_t i = 0; i < v.size() && i < 10; ++i) {
    std::snprintf(part, sizeof(part), "%s%s:%s x%u", i ? ", " : "", ZoneName(static_cast<int>(v[i].first >> 32) - 1),
                  Describe(static_cast<uint32_t>(v[i].first)).c_str(), v[i].second);
    text += part;
  }
  std::vector<std::pair<uint32_t, unsigned>> c(callers.begin(), callers.end());
  std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string calls;
  for (size_t i = 0; i < c.size() && i < 6; ++i) {
    std::snprintf(part, sizeof(part), "%s%06x x%u", i ? ", " : "", c[i].first, c[i].second);
    calls += part;
  }
  patch::Log("perf hitch frame %ld (%.1f ms) samples %u: %s | game callers: %s", h.frame, h.ms, n, text.c_str(),
             calls.c_str());
}

DWORD WINAPI SamplerMain(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  const int us = GetConfig().profile_sample_us;
  unsigned hitch_r = 0;
  DWORD last_dump = GetTickCount(), last_flush = GetTickCount(), last_mods = GetTickCount();
  if (us > 0) RefreshModules();
  while (!g_stop) {
    if (us > 0 && g_thread) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(us) * 10;
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, 100);
      else
        Sleep(1);
      TakeSample();
    } else {
      Sleep(50);
    }
    // Hitches whose frame is over.
    while (hitch_r != g_hitch_w.load()) {
      const Hitch h = g_hitch_q[hitch_r % 64];
      if (g_frame <= h.frame + 1) break;
      if (us > 0) ReportHitch(h);
      ++hitch_r;
    }
    const DWORD now = GetTickCount();
    if (now - last_flush > 1000) {
      FlushCsv();
      last_flush = now;
    }
    if (us > 0 && now - last_mods > 5000) {
      RefreshModules();
      last_mods = now;
    }
    if (us > 0 && now - last_dump > 15000) {
      DumpSamples();
      last_dump = now;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- summary

void WriteSummary(int64_t now) {
  Window& w = g_win;
  const size_t n = w.frame_ms.size();
  if (!n) return;
  const double secs = Ms(now - w.start) / 1000.0;
  double sum = 0;
  for (float f : w.frame_ms) sum += f;
  const double avg = sum / n;
  std::vector<float> f = w.frame_ms, b = w.busy_ms;
  const float p50 = Percentile(f, 0.5), p95 = Percentile(f, 0.95), p99 = Percentile(f, 0.99);
  const float mx = *std::max_element(f.begin(), f.end()), mn = *std::min_element(f.begin(), f.end());
  const float b50 = Percentile(b, 0.5), b95 = Percentile(b, 0.95);
  // Frame-to-frame variation: how uneven the pacing is.
  double jitter = 0;
  for (size_t i = 1; i < n; ++i) jitter += std::fabs(w.frame_ms[i] - w.frame_ms[i - 1]);
  jitter = n > 1 ? jitter / (n - 1) : 0;
  patch::Log("perf %.1fs %s: %zu frames, %.1f fps; frame ms avg %.2f min %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f, "
             "frame-to-frame change %.2f; busy (not waiting in Present/cap) p50 %.2f p95 %.2f; hitches %u; "
             "sim steps per frame 0:%u 1:%u 2:%u 3+:%u; gpu ms avg %.2f max %.2f",
             secs, w.race_frames * 2 > n ? "race" : "menu", n, n / secs, avg, mn, p50, p95, p99, mx, jitter, b50, b95,
             w.hitches, w.steps_hist[0], w.steps_hist[1], w.steps_hist[2], w.steps_hist[3],
             w.gpu_n ? w.gpu_sum / w.gpu_n : 0.0, w.gpu_max);
  std::string zones;
  char part[96];
  for (int z = 0; z < kZoneCount; ++z) {
    if (w.zc[z] == 0) continue;
    std::snprintf(part, sizeof(part), "%s%s %.3f (%.1f)", zones.empty() ? "" : ", ", kZoneNames[z], w.zt[z] / n,
                  w.zc[z] / n);
    zones += part;
  }
  patch::Log("perf   ms per frame (calls per frame): %s", zones.c_str());
  std::string counts;
  for (int c = 0; c < cCounterCount; ++c) {
    std::snprintf(part, sizeof(part), "%s%s %.1f", counts.empty() ? "" : ", ", kCounterNames[c], w.c[c] / n);
    counts += part;
  }
  patch::Log("perf   per frame: %s", counts.c_str());
  g_win = Window{};
  g_win.start = now;
  g_win.frame_ms.reserve(4096);
  g_win.busy_ms.reserve(4096);
}

void LogSystem() {
  int info[4] = {};
  char brand[49] = {};
  __cpuid(info, 0x80000000);
  if (static_cast<unsigned>(info[0]) >= 0x80000004) {
    for (int i = 0; i < 3; ++i) {
      __cpuid(info, 0x80000002 + i);
      std::memcpy(brand + i * 16, info, 16);
    }
  }
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
  const Config& c = GetConfig();
  patch::Log("perf: profiling on. CPU \"%s\", %lu logical processors; primary display %lux%lu @ %lu Hz; "
             "Renderer=D3D%d VSync=%d FrameCap=%d SimRate=%d SimSmooth=%d Antialiasing=%d GpuText=%d HudText=%d "
             "sample every %d us",
             brand, si.dwNumberOfProcessors, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, c.renderer,
             c.vsync, c.frame_cap, c.sim_rate, c.sim_smooth, c.msaa, c.gpu_text, c.hud_text, c.profile_sample_us);
}

}  // namespace

bool On() { return g_on; }

void ZoneEnter(Zone z, int64_t* start, int* prev) {
  if (!OnMain()) {
    *start = 0;
    return;
  }
  *start = Now();
  *prev = static_cast<int>(g_zone);
  g_zone = z;
  if (z == kTick) g_in_tick = true;
}

void ZoneLeave(Zone z, int64_t start, int prev) {
  g_cur.zt[z] += Now() - start;
  ++g_cur.zc[z];
  g_zone = prev;
  if (z == kTick) g_in_tick = false;
}

void Count(Counter c, unsigned n) {
  if (g_on && OnMain()) g_cur.c[c] += n;
}

void NoteDrawState(const void* tex0, const void* blend, const void* depth, const void* raster) {
  if (!g_on || !OnMain()) return;
  if (tex0 != g_last_tex) ++g_cur.c[cTexChanges];
  if (blend != g_last_blend || depth != g_last_depth || raster != g_last_raster) ++g_cur.c[cStateChanges];
  g_last_tex = tex0;
  g_last_blend = blend;
  g_last_depth = depth;
  g_last_raster = raster;
}

void NoteTarget(const void* rtv) {
  if (!g_on || !OnMain()) return;
  if (rtv != g_last_rtv) ++g_cur.c[cTargetChanges];
  g_last_rtv = rtv;
}

void GpuFrameTime(double ms) {
  if (!g_on) return;
  g_gpu_last = ms;
  g_win.gpu_sum += ms;
  g_win.gpu_max = std::max(g_win.gpu_max, ms);
  ++g_win.gpu_n;
}

void CloseFrame();

void FrameEnd() {
  if (!g_on || !OnMain()) return;
  if (g_in_tick) {
    g_frame_pending = true;
    return;
  }
  CloseFrame();
}

void TickEnd() {
  if (!g_on || !g_frame_pending) return;
  g_frame_pending = false;
  CloseFrame();
}

void CloseFrame() {
  const int64_t now = Now();
  if (!g_last_frame_end) {
    g_last_frame_end = now;
    g_win.start = now;
    g_cur = FrameAcc{};
    return;
  }
  const FrameAcc a = g_cur;
  g_cur = FrameAcc{};
  const float frame_ms = static_cast<float>(Ms(now - g_last_frame_end));
  g_last_frame_end = now;
  const float wait_ms = static_cast<float>(Ms(a.zt[kSwapPresent] + a.zt[kCapWait]));
  const float busy_ms = std::max(0.0f, frame_ms - wait_ms);
  const bool race = SimRacing();
  const LONG frame = g_frame;

  Window& w = g_win;
  w.frame_ms.push_back(frame_ms);
  w.busy_ms.push_back(busy_ms);
  for (int z = 0; z < kZoneCount; ++z) {
    w.zt[z] += Ms(a.zt[z]);
    w.zc[z] += a.zc[z];
  }
  for (int c = 0; c < cCounterCount; ++c) w.c[c] += a.c[c];
  ++w.steps_hist[std::min<unsigned>(a.c[cSteps], 3)];
  if (race) ++w.race_frames;

  // Hitch: much longer than the recent median.
  g_recent[g_recent_n++ % 240] = frame_ms;
  if ((g_recent_n & 15) == 0 || g_recent_n < 16) {
    const unsigned k = std::min<unsigned>(g_recent_n, 240);
    std::vector<float> r(g_recent, g_recent + k);
    g_median = Percentile(r, 0.5);
  }
  if (g_recent_n > 30 && frame_ms > std::max(g_median * 1.6f, g_median + 3.0f)) {
    ++w.hitches;
    if (g_hitch_logs < 300) {
      ++g_hitch_logs;
      std::string zones;
      char part[80];
      for (int z = 0; z < kZoneCount; ++z) {
        const double ms = Ms(a.zt[z]);
        if (ms < 0.3) continue;
        std::snprintf(part, sizeof(part), "%s%s %.2f/%u", zones.empty() ? "" : ", ", kZoneNames[z], ms, a.zc[z]);
        zones += part;
      }
      patch::Log("perf hitch frame %ld: %.2f ms (median %.2f), %s, steps %u, draws %u, uploads %u (%u KB), "
                 "readbacks %u, locks %u, tex creates %u; zones ms/calls: %s",
                 frame, frame_ms, g_median, race ? "race" : "menu", a.c[cSteps], a.c[cDraws], a.c[cUploads],
                 a.c[cUploadKB], a.c[cReadbacks], a.c[cLocks], a.c[cTexCreates], zones.c_str());
      const unsigned i = g_hitch_w.load();
      g_hitch_q[i % 64] = Hitch{frame, frame_ms};
      g_hitch_w.store(i + 1);
    }
  }

  // CSV line.
  char line[2048];
  int len = std::snprintf(line, sizeof(line), "%ld,%.4f,%d,%.3f,%.3f,%.3f", frame, Ms(now - g_start) / 1000.0,
                          race ? 1 : 0, frame_ms, busy_ms, g_gpu_last);
  for (int z = 0; z < kZoneCount && len < 1900; ++z) len += std::snprintf(line + len, 40, ",%.3f", Ms(a.zt[z]));
  for (int z = 0; z < kZoneCount && len < 1950; ++z) len += std::snprintf(line + len, 20, ",%u", a.zc[z]);
  for (int c = 0; c < cCounterCount && len < 2000; ++c) len += std::snprintf(line + len, 20, ",%u", a.c[c]);
  if (len < 2046) {
    line[len++] = '\n';
    EnterCriticalSection(&g_csv_cs);
    g_csv_buf.append(line, len);
    LeaveCriticalSection(&g_csv_cs);
  }

  g_frame = frame + 1;
  if (Ms(now - w.start) >= 5000.0) WriteSummary(now);
}

void Start() {
  const Config& c = GetConfig();
  if (!c.profile) return;
  QueryPerformanceFrequency(&g_freq);
  g_ms_per_tick = 1000.0 / static_cast<double>(g_freq.QuadPart);
  g_main_tid = GetCurrentThreadId();
  g_start = Now();
  InitializeCriticalSection(&g_csv_cs);
  InitializeCriticalSection(&g_map_cs);
  g_win.frame_ms.reserve(4096);
  g_win.busy_ms.reserve(4096);

  // Code ranges: the exe's .text, our whole image.
  const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    g_exe_lo = exe + sec[0].VirtualAddress;
    g_exe_hi = g_exe_lo + sec[0].Misc.VirtualSize;
  }
  HMODULE self = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCSTR>(&Start), &self);
  g_dll_base = reinterpret_cast<uintptr_t>(self);
  if (self) {
    // Our code section (whole pages, so the bytes before a return address
    // can always be read).
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_dll_base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(g_dll_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
        g_dll_lo = g_dll_base + sec[i].VirtualAddress;
        g_dll_hi = g_dll_lo + sec[i].Misc.VirtualSize;
        break;
      }
    }
  }
  g_stack_hi = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);

  OpenFiles();
  g_on = true;
  LogSystem();
  patch::Log("perf: exe code %08x-%08x, mcm2fix %08x-%08x, files perf-%s.csv / perf-samples-%s.txt",
             static_cast<unsigned>(g_exe_lo), static_cast<unsigned>(g_exe_hi), static_cast<unsigned>(g_dll_lo),
             static_cast<unsigned>(g_dll_hi), g_stamp, g_stamp);
  g_sampler = CreateThread(nullptr, 0, SamplerMain, nullptr, 0, nullptr);
}

void Stop() {
  if (!g_on) return;
  g_on = false;
  g_stop = true;
  // At process exit the sampler thread is already gone; write what we have.
  FlushCsv(true);
  if (GetConfig().profile_sample_us > 0) DumpSamples();
}

}  // namespace prof
