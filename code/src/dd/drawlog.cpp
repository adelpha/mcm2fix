// DrawLog=1: see drawlog.h.
#include "drawlog.h"

#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "../config.h"
#include "../framerate.h"
#include "../patch.h"
#include "surface.h"

namespace dd {
namespace {



constexpr int kSites = 3;          // game return addresses kept per draw (innermost first)
constexpr double kSummarySecs = 5.0;
constexpr int kMaxDumps = 4;       // whole frames listed per run
constexpr double kFirstDumpSecs = 8.0, kDumpEverySecs = 25.0;
constexpr uint32_t kMaxBatchVerts = 65535;  // 16-bit indices

uintptr_t g_exe_lo = 0, g_exe_hi = 0;

void InitRanges() {
  if (g_exe_lo) return;
  const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  g_exe_lo = exe + sec[0].VirtualAddress;
  g_exe_hi = g_exe_lo + sec[0].Misc.VirtualSize;
}

bool InExe(uintptr_t a) { return a >= g_exe_lo + 8 && a < g_exe_hi; }

// A return address: the bytes before it are a call instruction.
bool AfterCall(uintptr_t a) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(a);
  if (p[-5] == 0xE8) return true;
  if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return true;
  if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true;
  if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50) return true;
  if (p[-7] == 0xFF && p[-6] == 0x94) return true;
  if (p[-4] == 0xFF && p[-3] == 0x54) return true;
  return false;
}

// The game code on the stack above us, innermost first.
void GameSites(uint32_t out[kSites]) {
  for (int i = 0; i < kSites; ++i) out[i] = 0;
  auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
  const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
#ifdef _MSC_VER
  auto* p = reinterpret_cast<const uintptr_t*>(_AddressOfReturnAddress());
#else
  auto* p = reinterpret_cast<const uintptr_t*>(__builtin_frame_address(0));
#endif
  int n = 0;
  for (int i = 0; i < 1024 && n < kSites && reinterpret_cast<uintptr_t>(p + i + 1) <= top; ++i) {
    const uintptr_t v = p[i];
    if (!InExe(v) || !AfterCall(v)) continue;
    bool dup = false;
    for (int k = 0; k < n; ++k) dup |= out[k] == v;
    if (!dup) out[n++] = static_cast<uint32_t>(v);
  }
}

struct Entry {
  DrawLogRec r;
  uint32_t site[kSites];
  bool eligible;  // a shape a batching layer could merge
  bool merges;    // joins the draw before it
};

struct SiteStats {
  uint32_t site[kSites] = {};
  uint64_t draws = 0, merged = 0, eligible = 0, verts = 0, idx = 0;
  uint64_t tl = 0;
};

std::vector<Entry> g_frame;  // this frame's draws
uint32_t g_run_verts = 0;    // vertices in the current would-be batch

// Summary window.
struct Window {
  unsigned frames = 0;
  uint64_t draws = 0, after = 0, eligible = 0, eligible_after = 0, verts = 0, state_breaks = 0, shape_breaks = 0;
  uint64_t tex_breaks = 0, matrix_breaks = 0, other_breaks = 0;
  std::unordered_map<uint64_t, SiteStats> sites;
} g_w;
DWORD g_window_start = 0;
DWORD g_race_start = 0;
bool g_was_racing = false;
int g_dumps = 0;
double g_next_dump = kFirstDumpSecs;
uint64_t g_total_frames = 0;

uint64_t SiteKey(const uint32_t s[kSites]) {
  return (static_cast<uint64_t>(s[0]) << 32) ^ static_cast<uint64_t>(s[1]) * 0x9E3779B1u;
}

std::string SiteText(const uint32_t s[kSites]) {
  char b[64];
  if (!s[0]) return "?";
  if (s[2]) std::snprintf(b, sizeof(b), "%06x<%06x<%06x", s[0], s[1], s[2]);
  else if (s[1]) std::snprintf(b, sizeof(b), "%06x<%06x", s[0], s[1]);
  else std::snprintf(b, sizeof(b), "%06x", s[0]);
  return b;
}

std::string TexText(const Surface* t) {
  if (!t) return "-";
  const std::string& n = t->TextureName();
  char b[48];
  if (n.empty()) {
    std::snprintf(b, sizeof(b), "%ux%u@%p", t->Width(), t->Height(), static_cast<const void*>(t));
    return b;
  }
  return n.size() > 40 ? n.substr(0, 40) : n;
}

const char* PrimText(uint8_t p) {
  switch (p) {
    case 1: return "points";
    case 2: return "lines";
    case 3: return "linestrip";
    case 4: return "list";
    case 5: return "strip";
    case 6: return "fan";
  }
  return "?";
}

void DumpFrame() {
  const DWORD secs = (GetTickCount() - g_race_start) / 1000;
  unsigned after = 0;
  for (const Entry& e : g_frame) after += e.merges ? 0 : 1;
  patch::Log("drawlog: frame dump %d (%lu s into the race): %u draws, %u if consecutive same-state triangle draws were merged",
      g_dumps + 1, secs, static_cast<unsigned>(g_frame.size()), after);
  patch::Log("drawlog:   columns: draws  game call sites (innermost first)  api prim stride tl verts idx  texture[,tex1]  state");
  // One line per run of draws with the same site, texture and state.
  size_t i = 0;
  while (i < g_frame.size()) {
    size_t j = i + 1;
    const Entry& a = g_frame[i];
    uint64_t verts = a.r.count, idx = a.r.nidx;
    unsigned merges = 0;
    while (j < g_frame.size()) {
      const Entry& b = g_frame[j];
      if (b.site[0] != a.site[0] || b.site[1] != a.site[1] || b.r.tex0 != a.r.tex0 || b.r.state != a.r.state ||
          b.r.prim != a.r.prim || b.r.api != a.r.api)
        break;
      verts += b.r.count;
      idx += b.r.nidx;
      merges += b.merges ? 1 : 0;
      ++j;
    }
    static const char* kApi[] = {"dp", "dip", "strided", "vb"};
    std::string tex = TexText(a.r.tex0);
    if (a.r.tex1) tex += "," + TexText(a.r.tex1);
    patch::Log("drawlog:   #%-4u x%-3u %-22s %-7s %-9s %2u %s v%-6llu i%-6llu %-40s %08x%s%s", static_cast<unsigned>(i),
        static_cast<unsigned>(j - i), SiteText(a.site).c_str(), kApi[a.r.api & 3], PrimText(a.r.prim), a.r.stride,
        a.r.tl ? "TL" : (a.r.clip ? "CL" : "3D"), static_cast<unsigned long long>(verts),
        static_cast<unsigned long long>(idx), tex.c_str(), static_cast<unsigned>(a.r.state),
        a.merges ? " +prev" : "", merges ? (merges + 1 == j - i ? " (all merge)" : " (some merge)") : "");
    i = j;
  }
  ++g_dumps;
}

void Summary(DWORD now) {
  const double secs = (now - g_window_start) / 1000.0;
  if (!g_w.frames) return;
  const double f = g_w.frames;
  patch::Log("drawlog: %.1fs race, %u frames: per frame %.1f draws -> %.1f if merged; mergeable shape %.1f -> %.1f; "
      "%.0f verts; what split mergeable neighbours: state %.1f (texture %.1f, matrix %.1f, render state %.1f), "
      "vertex format %.1f",
      secs, g_w.frames, g_w.draws / f, g_w.after / f, g_w.eligible / f, g_w.eligible_after / f, g_w.verts / f,
      g_w.state_breaks / f, g_w.tex_breaks / f, g_w.matrix_breaks / f, g_w.other_breaks / f, g_w.shape_breaks / f);
  std::vector<const SiteStats*> v;
  for (auto& kv : g_w.sites) v.push_back(&kv.second);
  std::sort(v.begin(), v.end(), [](const SiteStats* a, const SiteStats* b) { return a->draws > b->draws; });
  const size_t n = std::min<size_t>(v.size(), 14);
  for (size_t i = 0; i < n; ++i) {
    const SiteStats& s = *v[i];
    patch::Log("drawlog:   site %-22s %6.1f draws/frame (%5.1f TL), %6.1f mergeable, %6.1f would join the one before; "
        "%7.0f verts %7.0f idx",
        SiteText(s.site).c_str(), s.draws / f, s.tl / f, s.eligible / f, s.merged / f, s.verts / f, s.idx / f);
  }
  g_w = Window{};
}

}  // namespace

bool DrawLogOn() { return GetConfig().draw_log; }

void DrawLogNote(const DrawLogRec& r) {
  if (!SimRacing()) return;
  InitRanges();
  Entry e;
  e.r = r;
  GameSites(e.site);
  e.eligible = !r.tl && !r.clip && (r.prim == 4 || r.prim == 6) && r.count <= kMaxBatchVerts;
  e.merges = false;
  if (!g_frame.empty()) {
    const Entry& p = g_frame.back();
    const bool shape = e.eligible && p.eligible && p.r.fvf_sig == r.fvf_sig;
    const bool state = p.r.state == r.state;
    if (shape && state && g_run_verts + r.count <= kMaxBatchVerts) {
      e.merges = true;
    } else if (e.eligible && p.eligible) {
      if (!shape) ++g_w.shape_breaks;
      else if (!state) {
        ++g_w.state_breaks;
        if (p.r.tex0 != r.tex0 || p.r.tex1 != r.tex1) ++g_w.tex_breaks;
        else if (p.r.matrices != r.matrices) ++g_w.matrix_breaks;
        else ++g_w.other_breaks;
      }
    }
  }
  g_run_verts = e.merges ? g_run_verts + r.count : r.count;
  g_frame.push_back(e);
  ++g_w.draws;
  g_w.verts += r.count;
  if (!e.merges) ++g_w.after;
  if (e.eligible) {
    ++g_w.eligible;
    if (!e.merges) ++g_w.eligible_after;
  }
  SiteStats& s = g_w.sites[SiteKey(e.site)];
  for (int k = 0; k < kSites; ++k) s.site[k] = e.site[k];
  ++s.draws;
  s.tl += r.tl ? 1 : 0;
  s.eligible += e.eligible ? 1 : 0;
  s.merged += e.merges ? 1 : 0;
  s.verts += r.count;
  s.idx += r.nidx;
}

void DrawLogFrameEnd() {
  if (!DrawLogOn()) return;
  const DWORD now = GetTickCount();
  const bool racing = SimRacing();
  if (racing && !g_was_racing) {
    g_race_start = now;
    g_window_start = now;
    g_next_dump = kFirstDumpSecs;
    g_w = Window{};
    patch::Log("drawlog: race started; summaries every %.0f s, whole-frame dumps from %.0f s (at most %d per run)",
        kSummarySecs, kFirstDumpSecs, kMaxDumps);
  }
  g_was_racing = racing;
  if (racing && !g_frame.empty()) {
    ++g_w.frames;
    ++g_total_frames;
    const double in_race = (now - g_race_start) / 1000.0;
    if (g_dumps < kMaxDumps && in_race >= g_next_dump) {
      DumpFrame();
      g_next_dump = in_race + kDumpEverySecs;
    }
    if (now - g_window_start >= static_cast<DWORD>(kSummarySecs * 1000)) {
      Summary(now);
      g_window_start = now;
    }
  }
  g_frame.clear();
  g_run_verts = 0;
}

}  // namespace dd
