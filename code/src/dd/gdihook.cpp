#include "gdihook.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#define CALLER_ADDRESS() _ReturnAddress()
#else
#define CALLER_ADDRESS() __builtin_return_address(0)
#endif

#include "../config.h"
#include "../prof.h"
#include "../patch.h"

namespace dd {
namespace {

using patch::Log;

struct Tracked {
  GdiTarget* target;
  UINT w, h;
  const char* what;
};

CRITICAL_SECTION g_cs;
bool g_cs_ready = false;
std::map<HDC, Tracked> g_dcs;

struct Lock {
  Lock() {
    if (g_cs_ready) EnterCriticalSection(&g_cs);
  }
  ~Lock() {
    if (g_cs_ready) LeaveCriticalSection(&g_cs);
  }
};

enum Fn { kTextOut, kDrawText, kFillRect, kFrameRect, kLineTo, kMoveTo, kClipRgn, kFnCount };
const char* const kFnName[kFnCount] = {"TextOut", "DrawText", "FillRect", "FrameRect", "LineTo", "MoveTo", "SelectClipRgn"};
unsigned g_count[kFnCount] = {};
unsigned g_other_dc[kFnCount] = {};
unsigned g_detail = 0;
constexpr unsigned kMaxDetail = 300;
DWORD g_last_report = 0;

bool Logging() { return GetConfig().gdi_log; }

// Looks up the import address table slot of dll!name in Mcm2.exe.
uintptr_t FindIatSlot(const char* dll, const char* name) {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return 0;
  for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), name) == 0)
        return reinterpret_cast<uintptr_t>(&slots->u1.Function);
    }
  }
  return 0;
}

template <typename F>
bool HookByName(const char* dll, const char* name, F replacement, F* original) {
  const uintptr_t slot = FindIatSlot(dll, name);
  if (!slot) {
    Log("gdi hooks: %s!%s not imported", dll, name);
    return false;
  }
  *original = reinterpret_cast<F>(patch::HookIat(slot, reinterpret_cast<void*>(replacement)));
  return true;
}

using TextOutFn = BOOL(WINAPI*)(HDC, int, int, LPCSTR, int);
using DrawTextFn = int(WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
using RectFn = int(WINAPI*)(HDC, const RECT*, HBRUSH);
using LineToFn = BOOL(WINAPI*)(HDC, int, int);
using MoveToFn = BOOL(WINAPI*)(HDC, int, int, LPPOINT);
using ClipFn = int(WINAPI*)(HDC, HRGN);
using CreateFontFn = HFONT(WINAPI*)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);
using CreateFontIndirectFn = HFONT(WINAPI*)(const LOGFONTA*);

TextOutFn o_text_out = nullptr;
DrawTextFn o_draw_text = nullptr;
RectFn o_fill_rect = nullptr, o_frame_rect = nullptr;
LineToFn o_line_to = nullptr;
MoveToFn o_move_to = nullptr;
ClipFn o_clip = nullptr;
CreateFontFn o_create_font = nullptr;
CreateFontIndirectFn o_create_font_indirect = nullptr;

void MaybeReport() {
  const DWORD now = GetTickCount();
  if (!g_last_report) g_last_report = now;
  if (now - g_last_report < 5000) return;
  g_last_report = now;
  if (!Logging()) return;
  char line[512];
  int n = 0;
  bool any = false;
  for (int i = 0; i < kFnCount; ++i) {
    if (!g_count[i] && !g_other_dc[i]) continue;
    any = true;
    n += std::snprintf(line + n, sizeof(line) - n, " %s %u", kFnName[i], g_count[i]);
    if (g_other_dc[i]) n += std::snprintf(line + n, sizeof(line) - n, " (+%u elsewhere)", g_other_dc[i]);
    if (n >= static_cast<int>(sizeof(line))) break;
  }
  if (any) Log("gdi: 5s:%s", line);
  std::memset(g_count, 0, sizeof(g_count));
  std::memset(g_other_dc, 0, sizeof(g_other_dc));
}

// Finds dc; counts the call either way. Returns null if not ours.
const Tracked* Find(HDC dc, Fn fn) {
  MaybeReport();
  auto it = g_dcs.find(dc);
  if (it == g_dcs.end()) {
    ++g_other_dc[fn];
    return nullptr;
  }
  ++g_count[fn];
  return &it->second;
}

void Touch(const Tracked* t, RECT r) {
  if (!t || !t->target) return;
  r.left = std::max<LONG>(r.left, 0);
  r.top = std::max<LONG>(r.top, 0);
  r.right = std::min<LONG>(r.right, static_cast<LONG>(t->w));
  r.bottom = std::min<LONG>(r.bottom, static_cast<LONG>(t->h));
  if (r.left >= r.right || r.top >= r.bottom) return;
  t->target->Touch(r);
}

bool Detail() { return Logging() && g_detail < kMaxDetail && ++g_detail; }

void DescribeFont(HDC dc, char* out, size_t n) {
  LOGFONTA lf{};
  HGDIOBJ f = GetCurrentObject(dc, OBJ_FONT);
  if (!f || !GetObjectA(f, sizeof(lf), &lf)) {
    std::snprintf(out, n, "font ?");
    return;
  }
  std::snprintf(out, n, "font \"%s\" h %ld w %ld weight %ld%s quality %u, colour %06lx, bk %s %06lx, extra %d, align %x",
                lf.lfFaceName, lf.lfHeight, lf.lfWidth, lf.lfWeight, lf.lfItalic ? " italic" : "",
                lf.lfQuality, static_cast<unsigned long>(GetTextColor(dc)),
                GetBkMode(dc) == TRANSPARENT ? "transparent" : "opaque", static_cast<unsigned long>(GetBkColor(dc)),
                GetTextCharacterExtra(dc), GetTextAlign(dc));
}

void Quote(char* out, size_t n, LPCSTR s, int len) {
  if (!s) {
    std::snprintf(out, n, "(null)");
    return;
  }
  if (len < 0) len = static_cast<int>(std::strlen(s));
  size_t j = 0;
  for (int i = 0; i < len && j + 2 < n; ++i) {
    const char c = s[i];
    out[j++] = (c == '\n' || c == '\r') ? '|' : (static_cast<unsigned char>(c) < 32 ? '?' : c);
  }
  out[j] = 0;
}

// A drawing call on a DC that is not one of ours (not from Surface::GetDC):
// log each place in the game that does this once, so it can be tracked down.
void LogOtherDc(const char* fn, HDC dc, void* caller, int x, int y, LPCSTR s, int len) {
  if (!Logging()) return;
  static std::vector<std::pair<void*, HDC>> seen;
  const bool null_dc = !dc;
  for (auto& e : seen)
    if (e.first == caller && (e.second == nullptr) == null_dc) return;
  if (seen.size() >= 64) return;
  seen.emplace_back(caller, dc);
  BITMAP bm{};
  const DWORD type = dc ? GetObjectType(dc) : 0;
  if (dc) {
    HGDIOBJ bmp = GetCurrentObject(dc, OBJ_BITMAP);
    if (bmp) GetObjectA(bmp, sizeof(bm), &bm);
  }
  char text[64];
  Quote(text, sizeof(text), s, len);
  HWND wnd = dc ? WindowFromDC(dc) : nullptr;
  Log("gdi: %s on a DC that is not a surface's: dc %p (type %lu, window %p, bitmap %ldx%ld %u bpp) at %d,%d \"%s\", "
      "called from %p", fn, static_cast<void*>(dc), type, static_cast<void*>(wnd), bm.bmWidth, bm.bmHeight,
      bm.bmBitsPixel, x, y, text, caller);
}

bool Divert(const Tracked* t, const GdiCall& c) { return t && t->target && t->target->Draw(c); }

BOOL WINAPI HookTextOut(HDC dc, int x, int y, LPCSTR s, int len) {
  PROF_ZONE(kGdiText);
  {
    Lock lock;
    const Tracked* t = Find(dc, kTextOut);
    if (t && Detail()) {
      char text[96], font[256];
      Quote(text, sizeof(text), s, len);
      DescribeFont(dc, font, sizeof(font));
      Log("gdi: TextOut on %s %ux%u at %d,%d \"%s\"; %s", t->what, t->w, t->h, x, y, text, font);
    }
    if (!t) LogOtherDc("TextOut", dc, CALLER_ADDRESS(), x, y, s, len);
    if (Divert(t, GdiCall{GdiFn::kTextOut, dc, x, y, s, len, nullptr, 0, nullptr})) return TRUE;
    if (t && t->target && s && len > 0) {
      SIZE sz{};
      GetTextExtentPoint32A(dc, s, len, &sz);
      TEXTMETRICA tm{};
      GetTextMetricsA(dc, &tm);
      const UINT align = GetTextAlign(dc);
      if (align & TA_UPDATECP) {
        POINT p{};
        GetCurrentPositionEx(dc, &p);
        x = p.x;
        y = p.y;
      }
      LONG left = x, top = y;
      if ((align & TA_CENTER) == TA_CENTER) left -= sz.cx / 2;
      else if (align & TA_RIGHT) left -= sz.cx;
      if ((align & TA_BASELINE) == TA_BASELINE) top -= tm.tmAscent;
      else if (align & TA_BOTTOM) top -= sz.cy;
      const LONG m = 4 + tm.tmOverhang + tm.tmMaxCharWidth / 2;
      Touch(t, RECT{left - m, top - 4, left + sz.cx + m, top + sz.cy + 4});
    }
  }
  return o_text_out(dc, x, y, s, len);
}

int WINAPI HookDrawText(HDC dc, LPCSTR s, int len, LPRECT rect, UINT fmt) {
  PROF_ZONE(kGdiText);
  {
    Lock lock;
    const Tracked* t = Find(dc, kDrawText);
    if (!t) LogOtherDc("DrawText", dc, CALLER_ADDRESS(), rect ? rect->left : 0, rect ? rect->top : 0, s, len);
    if (t && rect && !(fmt & DT_CALCRECT) && Detail()) {
      char text[96], font[256];
      Quote(text, sizeof(text), s, len);
      DescribeFont(dc, font, sizeof(font));
      Log("gdi: DrawText on %s %ux%u rect %ld,%ld-%ld,%ld fmt %x \"%s\"; %s", t->what, t->w, t->h, rect->left,
          rect->top, rect->right, rect->bottom, fmt, text, font);
    }
    if (rect && !(fmt & DT_CALCRECT) && Divert(t, GdiCall{GdiFn::kDrawText, dc, 0, 0, s, len, rect, fmt, nullptr})) {
      RECT calc = *rect;
      return o_draw_text(dc, s, len, &calc, fmt | DT_CALCRECT);
    }
    if (t && t->target && rect && !(fmt & DT_CALCRECT)) {
      RECT b = *rect;
      if (fmt & DT_NOCLIP) {
        RECT calc = *rect;
        o_draw_text(dc, s, len, &calc, fmt | DT_CALCRECT);
        const LONG cw = calc.right - calc.left + 8, ch = calc.bottom - calc.top + 8;
        b = RECT{rect->left - cw, rect->top - ch, rect->right + cw, rect->bottom + ch};
      }
      Touch(t, b);
    }
  }
  return o_draw_text(dc, s, len, rect, fmt);
}

void LogRect(const char* fn, const Tracked* t, const RECT* r, HBRUSH br) {
  if (!Detail()) return;
  LOGBRUSH lb{};
  if (reinterpret_cast<UINT_PTR>(br) > 32) GetObjectA(br, sizeof(lb), &lb);
  Log("gdi: %s on %s %ux%u rect %ld,%ld-%ld,%ld brush %p (style %u colour %06lx)", fn, t->what, t->w, t->h, r->left,
      r->top, r->right, r->bottom, static_cast<void*>(br), lb.lbStyle, static_cast<unsigned long>(lb.lbColor));
}

int WINAPI HookFillRect(HDC dc, const RECT* r, HBRUSH br) {
  {
    Lock lock;
    const Tracked* t = Find(dc, kFillRect);
    if (!t) LogOtherDc("FillRect", dc, CALLER_ADDRESS(), r ? r->left : 0, r ? r->top : 0, nullptr, 0);
    if (t && r) {
      LogRect("FillRect", t, r, br);
      if (Divert(t, GdiCall{GdiFn::kFillRect, dc, 0, 0, nullptr, 0, r, 0, br})) return 1;
      Touch(t, *r);
    }
  }
  return o_fill_rect(dc, r, br);
}

int WINAPI HookFrameRect(HDC dc, const RECT* r, HBRUSH br) {
  {
    Lock lock;
    const Tracked* t = Find(dc, kFrameRect);
    if (!t) LogOtherDc("FrameRect", dc, CALLER_ADDRESS(), r ? r->left : 0, r ? r->top : 0, nullptr, 0);
    if (t && r) {
      LogRect("FrameRect", t, r, br);
      if (Divert(t, GdiCall{GdiFn::kFrameRect, dc, 0, 0, nullptr, 0, r, 0, br})) return 1;
      Touch(t, *r);
    }
  }
  return o_frame_rect(dc, r, br);
}

BOOL WINAPI HookLineTo(HDC dc, int x, int y) {
  {
    Lock lock;
    const Tracked* t = Find(dc, kLineTo);
    if (!t) LogOtherDc("LineTo", dc, CALLER_ADDRESS(), x, y, nullptr, 0);
    if (Divert(t, GdiCall{GdiFn::kLineTo, dc, x, y, nullptr, 0, nullptr, 0, nullptr})) {
      MoveToEx(dc, x, y, nullptr);
      return TRUE;
    }
    if (t) {
      POINT p{};
      GetCurrentPositionEx(dc, &p);
      LOGPEN lp{};
      HGDIOBJ pen = GetCurrentObject(dc, OBJ_PEN);
      if (pen) GetObjectA(pen, sizeof(lp), &lp);
      const LONG m = 2 + std::max<LONG>(lp.lopnWidth.x, 1);
      Touch(t, RECT{std::min<LONG>(p.x, x) - m, std::min<LONG>(p.y, y) - m, std::max<LONG>(p.x, x) + m,
                    std::max<LONG>(p.y, y) + m});
      if (Detail())
        Log("gdi: LineTo on %s %ux%u %ld,%ld -> %d,%d pen style %u width %ld colour %06lx", t->what, t->w, t->h,
            p.x, p.y, x, y, lp.lopnStyle, lp.lopnWidth.x, static_cast<unsigned long>(lp.lopnColor));
    }
  }
  return o_line_to(dc, x, y);
}

BOOL WINAPI HookMoveTo(HDC dc, int x, int y, LPPOINT old) {
  {
    Lock lock;
    Find(dc, kMoveTo);
  }
  return o_move_to(dc, x, y, old);
}

int WINAPI HookClip(HDC dc, HRGN rgn) {
  {
    Lock lock;
    const Tracked* t = Find(dc, kClipRgn);
    if (t && t->target) t->target->ClipChanged();
    if (t && Detail()) {
      RECT b{};
      if (rgn) GetRgnBox(rgn, &b);
      Log("gdi: SelectClipRgn on %s %ux%u %s %ld,%ld-%ld,%ld", t->what, t->w, t->h, rgn ? "box" : "none", b.left,
          b.top, b.right, b.bottom);
    }
  }
  return o_clip(dc, rgn);
}

unsigned g_fonts_logged = 0;

HFONT WINAPI HookCreateFont(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline,
                            DWORD strike, DWORD charset, DWORD out_prec, DWORD clip_prec, DWORD quality, DWORD pitch,
                            LPCSTR face) {
  HFONT f = o_create_font(h, w, esc, orient, weight, italic, underline, strike, charset, out_prec, clip_prec,
                          quality, pitch, face);
  if (Logging() && g_fonts_logged < 100) {
    ++g_fonts_logged;
    Log("gdi: CreateFont \"%s\" h %d w %d weight %d%s quality %lu charset %lu -> %p", face ? face : "", h, w, weight,
        italic ? " italic" : "", quality, charset, static_cast<void*>(f));
  }
  return f;
}

HFONT WINAPI HookCreateFontIndirect(const LOGFONTA* lf) {
  HFONT f = o_create_font_indirect(lf);
  if (Logging() && g_fonts_logged < 100 && lf) {
    ++g_fonts_logged;
    Log("gdi: CreateFontIndirect \"%s\" h %ld w %ld weight %ld%s quality %u charset %u -> %p", lf->lfFaceName,
        lf->lfHeight, lf->lfWidth, lf->lfWeight, lf->lfItalic ? " italic" : "", lf->lfQuality, lf->lfCharSet,
        static_cast<void*>(f));
  }
  return f;
}

}  // namespace

void InstallGdiHooks() {
  static bool done = false;
  if (done) return;
  done = true;
  InitializeCriticalSection(&g_cs);
  g_cs_ready = true;
  int ok = 0;
  ok += HookByName("GDI32.dll", "TextOutA", &HookTextOut, &o_text_out);
  ok += HookByName("USER32.dll", "DrawTextA", &HookDrawText, &o_draw_text);
  ok += HookByName("USER32.dll", "FillRect", &HookFillRect, &o_fill_rect);
  ok += HookByName("USER32.dll", "FrameRect", &HookFrameRect, &o_frame_rect);
  ok += HookByName("GDI32.dll", "LineTo", &HookLineTo, &o_line_to);
  ok += HookByName("GDI32.dll", "MoveToEx", &HookMoveTo, &o_move_to);
  ok += HookByName("GDI32.dll", "SelectClipRgn", &HookClip, &o_clip);
  ok += HookByName("GDI32.dll", "CreateFontA", &HookCreateFont, &o_create_font);
  ok += HookByName("GDI32.dll", "CreateFontIndirectA", &HookCreateFontIndirect, &o_create_font_indirect);
  Log("gdi hooks: %d of 9 applied (GdiLog=%d)", ok, GetConfig().gdi_log ? 1 : 0);
}

void GdiTrack(HDC dc, GdiTarget* target, UINT w, UINT h, const char* what) {
  if (!dc) return;
  Lock lock;
  auto it = g_dcs.find(dc);
  if (it != g_dcs.end() && it->second.target && !target) return;  // keep the more specific entry
  g_dcs[dc] = Tracked{target, w, h, what};
}

void GdiUntrack(HDC dc) {
  Lock lock;
  g_dcs.erase(dc);
}

bool GdiTracked(HDC dc) {
  Lock lock;
  return g_dcs.count(dc) != 0;
}

void GdiReportStats() {
  Lock lock;
  MaybeReport();
}

}  // namespace dd
