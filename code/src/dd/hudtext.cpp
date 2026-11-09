#include "hudtext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../config.h"
#include "../patch.h"
#include "backend.h"
#include "gdihook.h"
#include "gputext.h"
#include "surface.h"

namespace dd {
namespace {

// The game's HUD panel object (an overlay; thiscall, ecx).
constexpr uintptr_t kConvert = 0x4bafd0;  // (int first_row, int rows, pixel colour); ret 0xc
constexpr uintptr_t kRestore = 0x4bae60;  // (const RECT*); ret 4
constexpr uintptr_t kLayout = 0x4baad0;   // (); lays out the quad below
constexpr uintptr_t kPanelTexture = 0x2c;  // texture wrapper: +0x14 width, +0x18 height, +0x70 IDDS7
constexpr uintptr_t kPanelScratch = 0x34;  // scratch texture wrapper
constexpr uintptr_t kOwner = 0x18;         // view: +8 camera (+0x1a8 w, +0x1ac h), else +0xc w, +0x10 h
constexpr uintptr_t kVerts = 0x44;         // 4 TL vertices, 0x20 bytes: x y z rhw diffuse specular u v
constexpr uintptr_t kScaledUnits = 0xe4;   // nonzero: rectangle in 640x480 units, else pixels
constexpr uintptr_t kRect = 0xe8;          // x1 y1 x2 y2
constexpr uintptr_t kWrapperW = 0x14, kWrapperH = 0x18, kWrapperSurface = 0x70;
constexpr uintptr_t kConvertCalls[] = {0x51f43c, 0x51faa7, 0x520067, 0x5204b0, 0x5236d9, 0x523824, 0x52407b};
constexpr uintptr_t kRestoreCalls[] = {0x51f2f7, 0x51f952, 0x51ff2c, 0x52039e, 0x523473,
                                       0x5238f0, 0x523e9a, 0x523f99, 0x523fc1};
// Return addresses of the panels' GetDC calls on their scratch textures.
constexpr uintptr_t kScratchGetDC[] = {0x51f311, 0x51f96c, 0x51ff46, 0x5203b8, 0x52348d, 0x5236ec, 0x523fdb};

using ConvertFn = void(__fastcall*)(uintptr_t self, void* edx, int first_row, int rows, DWORD colour);
using RestoreFn = void(__fastcall*)(uintptr_t self, void* edx, const RECT* r);
using LayoutFn = void(__fastcall*)(uintptr_t self, void* edx);
const ConvertFn g_convert = reinterpret_cast<ConvertFn>(kConvert);
const RestoreFn g_restore = reinterpret_cast<RestoreFn>(kRestore);

// One text call, in scratch (= panel) pixels.
struct Item {
  bool draw_text = false;  // DrawText (rect, fmt), else TextOut (x, y)
  std::string s;
  RECT rect{};
  int x = 0, y = 0;
  UINT fmt = 0, align = 0;
  int extra = 0;
  int ascent = 0;  // the game font's, for lining up the baseline
  LOGFONTA font{};
  RECT box{};  // where its pixels are
  COLORREF colour = RGB(255, 255, 255);
};

struct Panel {
  std::vector<Item> items;
  bool laid_out = false;
  int orig[4] = {}, written[4] = {};  // the game's rectangle and what we stored
  bool have_rect = false;
  unsigned draws = 0;
};

std::map<uintptr_t, Panel> g_panels;
std::map<const Surface*, std::vector<Item>> g_pending;  // captured, not yet converted
std::map<HDC, Surface*> g_scratch_dc;
std::set<const Surface*> g_scratch;
std::map<const Surface*, uintptr_t> g_panel_tex;  // panel texture -> panel
// A panel texture copied into another texture (the game draws the copy).
struct Copy {
  uintptr_t panel;
  RECT sr, dr;
};
std::map<const Surface*, std::vector<Copy>> g_copies;

struct Stats {
  unsigned captured = 0, converted = 0, moved = 0, wipes = 0, wiped = 0, draws = 0, drawn = 0, no_dc = 0;
  unsigned labels = 0, label_skips = 0, copies = 0;
} g_st;
DWORD g_last_report = 0;

bool Active() { return GetConfig().gpu_text && GetConfig().hud_text && GpuTextActive(); }

void Report() {
  const DWORD now = GetTickCount();
  if (!g_last_report) g_last_report = now;
  if (now - g_last_report < 5000) return;
  g_last_report = now;
  if (g_st.captured || g_st.converted || g_st.wipes || g_st.draws) {
    size_t items = 0;
    for (auto& p : g_panels) items += p.second.items.size();
    Log("hud text 5s: %u text calls captured, %u conversions (%u texts to panels), %u wipes (%u texts removed), "
        "%u panel draws, %u label draws (%u without text there), %u texts drawn, %u without a GPU text DC, "
        "%u panel copies; %u panels hold %u texts",
        g_st.captured, g_st.converted, g_st.moved, g_st.wipes, g_st.wiped, g_st.draws, g_st.labels, g_st.label_skips,
        g_st.drawn, g_st.no_dc, g_st.copies,
        static_cast<unsigned>(g_panels.size()), static_cast<unsigned>(items));
  }
  // A panel with text whose quad was never seen drawn: say where it is.
  for (auto& p : g_panels) {
    if (!p.second.items.empty() && !p.second.draws) {
      static unsigned logged = 0;
      if (++logged <= 5) {
        const float* v = reinterpret_cast<const float*>(p.first + kVerts);
        Log("hud text: panel %08x has %u texts but no draw matched its quad (%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f)",
            static_cast<unsigned>(p.first), static_cast<unsigned>(p.second.items.size()), v[0], v[1], v[8], v[9],
            v[16], v[17], v[24], v[25]);
      }
    }
    p.second.draws = 0;
  }
  g_st = Stats{};
}

uintptr_t Wrapper(uintptr_t self, uintptr_t field) {
  return self ? *reinterpret_cast<const uintptr_t*>(self + field) : 0;
}

Surface* SurfaceAt(uintptr_t self, uintptr_t field) {
  const uintptr_t w = Wrapper(self, field);
  if (!w) return nullptr;
  auto* dds = *reinterpret_cast<IDirectDrawSurface7* const*>(w + kWrapperSurface);
  return dds ? Surface::From(dds) : nullptr;
}

bool Intersects(const RECT& a, const RECT& b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

float Scale() { return std::max(1.0f, std::min(4.0f, GetConfig().hud_scale)); }

// The panel's quad was laid out at its normal size; make it Scale() times
// larger, keeping it against the nearest screen edges (distance scaled too).
void ScaleRect(uintptr_t inst, Panel& p) {
  int* r = reinterpret_cast<int*>(inst + kRect);
  int o[4] = {r[0], r[1], r[2], r[3]};
  if (p.have_rect && std::equal(r, r + 4, p.written)) std::copy(p.orig, p.orig + 4, o);
  int vw = 640, vh = 480;
  if (!*reinterpret_cast<const int*>(inst + kScaledUnits)) {
    const uintptr_t owner = Wrapper(inst, kOwner);
    if (!owner) return;
    const uintptr_t cam = *reinterpret_cast<const uintptr_t*>(owner + 8);
    vw = cam ? *reinterpret_cast<const int*>(cam + 0x1a8) : *reinterpret_cast<const int*>(owner + 0xc);
    vh = cam ? *reinterpret_cast<const int*>(cam + 0x1ac) : *reinterpret_cast<const int*>(owner + 0x10);
    if (vw <= 0 || vh <= 0) return;
  }
  const float k = Scale();
  int n[4];
  auto axis = [k](int a, int b, int size, int* na, int* nb) {
    const int len = static_cast<int>(std::lround((b - a) * k));
    const int mid = (a + b) / 2;
    if (std::abs(mid - size / 2) <= size / 6) {  // centred: stays centred
      *na = static_cast<int>(std::lround(size / 2.0f - (size / 2.0f - a) * k));
      *nb = *na + len;
    } else if (mid < size / 2) {  // nearer the left / top edge
      *na = static_cast<int>(std::lround(a * k));
      *nb = *na + len;
    } else {
      *nb = size - static_cast<int>(std::lround((size - b) * k));
      *na = *nb - len;
    }
  };
  axis(o[0], o[2], vw, &n[0], &n[2]);
  axis(o[1], o[3], vh, &n[1], &n[3]);
  std::copy(o, o + 4, p.orig);
  std::copy(n, n + 4, p.written);
  std::copy(n, n + 4, r);
  p.have_rect = true;
  static unsigned logged = 0;
  if (++logged <= 10)
    Log("hud text: panel %08x (%s units, view %dx%d) %d,%d-%d,%d laid out x%.2f as %d,%d-%d,%d",
        static_cast<unsigned>(inst), vw == 640 && vh == 480 ? "640x480" : "pixel", vw, vh, o[0], o[1], o[2], o[3], k,
        n[0], n[1], n[2], n[3]);
}

// First sight of a text panel: lay it out again, now scaled.
Panel& Register(uintptr_t self) {
  Panel& p = g_panels[self];
  if (Surface* pt = SurfaceAt(self, kPanelTexture)) g_panel_tex[pt] = self;
  if (!p.laid_out) {
    p.laid_out = true;
    Surface* t = SurfaceAt(self, kPanelTexture);
    Log("hud text: text panel %08x (vtable %08x, texture %ux%u)", static_cast<unsigned>(self),
        *reinterpret_cast<const unsigned*>(self), t ? t->Width() : 0, t ? t->Height() : 0);
    if (Scale() != 1.0f) reinterpret_cast<LayoutFn>(kLayout)(self, nullptr);
  }
  return p;
}

COLORREF PixelColour(const Surface* t, DWORD px) {
  const PixelLayout& l = t->Layout();
  int c[3] = {255, 255, 255};
  for (int i = 0; i < 3; ++i) {
    const DWORD m = l.mask[i];
    if (!m) continue;
    DWORD shift = 0;
    while (!((m >> shift) & 1)) ++shift;
    const DWORD max = m >> shift;
    c[i] = static_cast<int>(((px & m) >> shift) * 255 / max);
  }
  return RGB(c[0], c[1], c[2]);
}

// The game's text calls on a scratch texture: kept, not drawn.
class Capture : public GdiTarget {
 public:
  bool Draw(const GdiCall& call) override {
    if (call.fn != GdiFn::kTextOut && call.fn != GdiFn::kDrawText) return false;
    auto it = g_scratch_dc.find(call.dc);
    if (it == g_scratch_dc.end() || !call.s) return false;
    HDC dc = call.dc;
    Item item;
    item.align = GetTextAlign(dc);
    if (item.align & TA_UPDATECP) return false;  // not used by the panels; let GDI have it
    const int len = call.len >= 0 ? call.len : static_cast<int>(std::strlen(call.s));
    item.s.assign(call.s, static_cast<size_t>(std::max(len, 0)));
    GetObjectA(GetCurrentObject(dc, OBJ_FONT), sizeof(item.font), &item.font);
    item.extra = GetTextCharacterExtra(dc);
    {
      TEXTMETRICA tm{};
      GetTextMetricsA(dc, &tm);
      item.ascent = tm.tmAscent;
    }
    if (call.fn == GdiFn::kTextOut) {
      SIZE ext{};
      GetTextExtentPoint32A(dc, item.s.c_str(), len, &ext);
      TEXTMETRICA tm{};
      GetTextMetricsA(dc, &tm);
      item.x = call.x;
      item.y = call.y;
      LONG left = call.x, top = call.y;
      if ((item.align & TA_CENTER) == TA_CENTER) left -= ext.cx / 2;
      else if (item.align & TA_RIGHT) left -= ext.cx;
      if ((item.align & TA_BASELINE) == TA_BASELINE) top -= tm.tmAscent;
      else if (item.align & TA_BOTTOM) top -= ext.cy;
      item.box = {left, top, left + ext.cx, top + ext.cy};
    } else {
      if (!call.rect) return false;
      item.draw_text = true;
      item.rect = *call.rect;
      item.fmt = call.fmt;
      RECT calc = *call.rect;
      DrawTextA(dc, item.s.c_str(), len, &calc, (call.fmt & ~DT_NOCLIP) | DT_CALCRECT);
      const LONG w = calc.right - calc.left, h = calc.bottom - calc.top;
      LONG left = call.rect->left, top = call.rect->top;
      if (call.fmt & DT_CENTER) left = (call.rect->left + call.rect->right - w) / 2;
      else if (call.fmt & DT_RIGHT) left = call.rect->right - w;
      if ((call.fmt & DT_SINGLELINE) && (call.fmt & DT_VCENTER)) top = (call.rect->top + call.rect->bottom - h) / 2;
      else if ((call.fmt & DT_SINGLELINE) && (call.fmt & DT_BOTTOM)) top = call.rect->bottom - h;
      item.box = {left, top, left + w, top + h};
    }
    ++g_st.captured;
    static unsigned logged = 0;
    if (++logged <= 30)
      Log("hud text: captured %s \"%s\" font \"%s\" %ld%s, at %ld,%ld-%ld,%ld (fmt %x align %x)",
          item.draw_text ? "DrawText" : "TextOut", item.s.c_str(), item.font.lfFaceName, item.font.lfHeight,
          item.font.lfWeight >= FW_BOLD ? " bold" : "", item.box.left, item.box.top, item.box.right, item.box.bottom,
          item.fmt, item.align);
    auto& pending = g_pending[it->second];
    // Text drawn again over the same place replaces what was there.
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&](const Item& o) { return Intersects(o.box, item.box); }),
                  pending.end());
    if (pending.size() < 256) pending.push_back(std::move(item));
    return true;
  }
};
Capture g_capture;

void __fastcall ConvertHook(uintptr_t self, void* edx, int first_row, int rows, DWORD colour) {
  Surface* t = Active() ? SurfaceAt(self, kPanelTexture) : nullptr;
  Surface* s = t ? SurfaceAt(self, kPanelScratch) : nullptr;
  if (s) {
    Report();
    g_scratch.insert(s);
    Panel& p = Register(self);
    ++g_st.converted;
    const LONG y0 = first_row, y1 = first_row + rows;
    const COLORREF c = PixelColour(t, colour);
    auto& pending = g_pending[s];
    unsigned moved = 0;
    for (auto it = pending.begin(); it != pending.end();) {
      const LONG mid = (it->box.top + it->box.bottom) / 2;
      if (mid < y0 || mid >= y1) {
        ++it;
        continue;
      }
      it->colour = c;
      const RECT box = it->box;
      p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                   [&](const Item& o) { return Intersects(o.box, box); }),
                    p.items.end());
      p.items.push_back(std::move(*it));
      it = pending.erase(it);
      ++moved;
    }
    g_st.moved += moved;
    static unsigned logged = 0;
    if (++logged <= 20)
      Log("hud text: panel %08x rows %d-%d converted in %06lx (pixel %08lx): %u texts, panel now has %u",
          static_cast<unsigned>(self), y0, y1, static_cast<unsigned long>(c), static_cast<unsigned long>(colour),
          moved, static_cast<unsigned>(p.items.size()));
  }
  // The game still converts the rest (lines, boxes) as before.
  g_convert(self, edx, first_row, rows, colour);
}

void __fastcall RestoreHook(uintptr_t self, void* edx, const RECT* rect) {
  g_restore(self, edx, rect);
  if (!Active() || !rect || !SurfaceAt(self, kPanelScratch)) return;
  Report();
  Panel& p = Register(self);
  ++g_st.wipes;
  const size_t before = p.items.size();
  p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                               [&](const Item& o) { return Intersects(o.box, *rect); }),
                p.items.end());
  g_st.wiped += static_cast<unsigned>(before - p.items.size());
}

bool PatchCall(uintptr_t site, uintptr_t original, const void* hook) {
  uint8_t expect[5] = {0xe8};
  const int32_t rel = static_cast<int32_t>(original - (site + 5));
  std::memcpy(expect + 1, &rel, 4);
  uint8_t repl[5] = {0xe8};
  const int32_t rel2 = static_cast<int32_t>(reinterpret_cast<uintptr_t>(hook) - (site + 5));
  std::memcpy(repl + 1, &rel2, 4);
  return patch::Replace("hud text call", site, expect, 5, repl, 5);
}

struct FontKey {
  LOGFONTA lf;
  bool operator<(const FontKey& o) const { return std::memcmp(&lf, &o.lf, sizeof(lf)) < 0; }
};
std::map<FontKey, HFONT> g_fonts;

HFONT ScaledFont(const LOGFONTA& lf, float kx, float ky) {
  FontKey key{lf};
  key.lf.lfHeight = static_cast<LONG>(std::lround(lf.lfHeight * ky));
  key.lf.lfWidth = static_cast<LONG>(std::lround(lf.lfWidth * kx));
  key.lf.lfQuality = ANTIALIASED_QUALITY;
  auto it = g_fonts.find(key);
  if (it != g_fonts.end()) return it->second;
  HFONT f = CreateFontIndirectA(&key.lf);
  g_fonts[key] = f;
  return f;
}

// Draw the texts of a panel that fall in `region` (panel pixels) at screen
// X = ax + x * kx, Y = ay + y * ky, clipped to `clip` (render target pixels).
void DrawItems(Surface* rt, uintptr_t self, const Panel& p, const RECT& region, float ax, float ay, float kx,
               float ky, const RECT& clip_rect, const char* what) {
  auto X = [&](LONG x) { return static_cast<LONG>(std::lround(ax + x * kx)); };
  auto Y = [&](LONG y) { return static_cast<LONG>(std::lround(ay + y * ky)); };
  bool any = false;
  for (const Item& it : p.items) any = any || Intersects(it.box, region);
  if (!any) {
    ++g_st.label_skips;
    return;
  }
  HDC dc = rt->BeginGpuText();
  if (!dc) {
    ++g_st.no_dc;
    return;
  }
  HRGN clip = CreateRectRgn(clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom);
  SelectClipRgn(dc, clip);
  DeleteObject(clip);
  SetBkMode(dc, TRANSPARENT);
  unsigned n = 0;
  for (const Item& it : p.items) {
    if (!Intersects(it.box, region)) continue;
    SelectObject(dc, ScaledFont(it.font, kx, ky));
    SetTextColor(dc, it.colour);
    SetTextAlign(dc, it.align);
    SetTextCharacterExtra(dc, static_cast<int>(std::lround(it.extra * kx)));
    // The scaled font can have a different share of space above its letters:
    // put its baseline where the game's font had it, scaled.
    TEXTMETRICA tm{};
    GetTextMetricsA(dc, &tm);
    const bool top_aligned = it.draw_text ? !(it.fmt & (DT_VCENTER | DT_BOTTOM))
                                          : (it.align & (TA_BASELINE | TA_BOTTOM)) == TA_TOP;
    const LONG dy = top_aligned ? static_cast<LONG>(std::lround(it.ascent * ky)) - tm.tmAscent : 0;
    GdiCall call{};
    call.dc = dc;
    call.s = it.s.c_str();
    call.len = static_cast<int>(it.s.size());
    RECT r{};
    if (it.draw_text) {
      call.fn = GdiFn::kDrawText;
      r = {X(it.rect.left), Y(it.rect.top) + dy, X(it.rect.right), Y(it.rect.bottom) + dy};
      call.rect = &r;
      call.fmt = it.fmt;
    } else {
      call.fn = GdiFn::kTextOut;
      call.x = X(it.x);
      call.y = Y(it.y) + dy;
    }
    if (GpuTextRun(call)) ++g_st.drawn;
    ++n;
  }
  SelectObject(dc, GetStockObject(SYSTEM_FONT));
  SelectClipRgn(dc, nullptr);
  GpuTextEndDC(dc);
  static unsigned logged = 0;
  if (++logged <= 12)
    Log("hud text: %s of panel %08x: panel pixels %ld,%ld-%ld,%ld at screen %ld,%ld-%ld,%ld, %.2f x %.2f per pixel, "
        "%u texts",
        what, static_cast<unsigned>(self), region.left, region.top, region.right, region.bottom, clip_rect.left,
        clip_rect.top, clip_rect.right, clip_rect.bottom, kx, ky, n);
}

// The panel's own quad (its layout at +0x44).
void DrawPanel(Surface* rt, uintptr_t self, Panel& p) {
  const uintptr_t tw = Wrapper(self, kPanelTexture);
  if (!tw) return;
  const int texw = *reinterpret_cast<const int*>(tw + kWrapperW), texh = *reinterpret_cast<const int*>(tw + kWrapperH);
  const float* v = reinterpret_cast<const float*>(self + kVerts);  // 8 floats per vertex
  // Texel -> screen, from the two vertices furthest apart on each axis.
  int ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;
  for (int i = 1; i < 4; ++i) {
    if (v[i * 8] < v[ix0 * 8]) ix0 = i;
    if (v[i * 8] > v[ix1 * 8]) ix1 = i;
    if (v[i * 8 + 1] < v[iy0 * 8 + 1]) iy0 = i;
    if (v[i * 8 + 1] > v[iy1 * 8 + 1]) iy1 = i;
  }
  const float du = (v[ix1 * 8 + 6] - v[ix0 * 8 + 6]) * texw, dv = (v[iy1 * 8 + 7] - v[iy0 * 8 + 7]) * texh;
  if (texw <= 0 || texh <= 0 || std::fabs(du) < 1 || std::fabs(dv) < 1) return;
  const float kx = (v[ix1 * 8] - v[ix0 * 8]) / du, ky = (v[iy1 * 8 + 1] - v[iy0 * 8 + 1]) / dv;
  const float ox = v[ix0 * 8] - v[ix0 * 8 + 6] * texw * kx, oy = v[iy0 * 8 + 1] - v[iy0 * 8 + 7] * texh * ky;
  const RECT clip{static_cast<LONG>(std::floor(v[ix0 * 8])), static_cast<LONG>(std::floor(v[iy0 * 8 + 1])),
                  static_cast<LONG>(std::ceil(v[ix1 * 8])), static_cast<LONG>(std::ceil(v[iy1 * 8 + 1]))};
  DrawItems(rt, self, p, RECT{-100000, -100000, 100000, 100000}, ox, oy, kx, ky, clip, "quad");
}

// The text panel whose own quad covers exactly this screen rectangle.
std::pair<const uintptr_t, Panel>* QuadOf(float x0, float y0, float x1, float y1) {
  for (auto& e : g_panels) {
    const float* v = reinterpret_cast<const float*>(e.first + kVerts);
    float px0 = 1e9f, py0 = 1e9f, px1 = -1e9f, py1 = -1e9f;
    for (int i = 0; i < 4; ++i) {
      px0 = std::min(px0, v[i * 8]), px1 = std::max(px1, v[i * 8]);
      py0 = std::min(py0, v[i * 8 + 1]), py1 = std::max(py1, v[i * 8 + 1]);
    }
    if (std::fabs(px0 - x0) <= 0.5f && std::fabs(px1 - x1) <= 0.5f && std::fabs(py0 - y0) <= 0.5f &&
        std::fabs(py1 - y1) <= 0.5f)
      return &e;
  }
  return nullptr;
}

// Which panel (and part of it) a texture shows: the panel texture itself or
// a copy of it.
bool FindSource(Surface* tex, const RECT& texels, uintptr_t* panel, const Copy** copy) {
  auto pt = g_panel_tex.find(tex);
  if (pt != g_panel_tex.end()) {
    *panel = pt->second;
    *copy = nullptr;
    return true;
  }
  auto it = g_copies.find(tex);
  if (it == g_copies.end()) return false;
  for (const Copy& c : it->second)
    if (Intersects(c.dr, texels)) {
      *panel = c.panel;
      *copy = &c;
      return true;
    }
  return false;
}

}  // namespace

bool HudTextScratchDC(HDC dc, Surface* s, void* caller) {
  if (!dc || !s || !Active()) return false;
  const uintptr_t ret = reinterpret_cast<uintptr_t>(caller);
  const bool site = std::find(std::begin(kScratchGetDC), std::end(kScratchGetDC), ret) != std::end(kScratchGetDC);
  if (!site && !g_scratch.count(s)) return false;
  if (g_scratch.insert(s).second)
    Log("hud text: %ux%u scratch texture (GetDC from %08x): its text is captured", s->Width(), s->Height(),
        static_cast<unsigned>(ret));
  g_scratch_dc[dc] = s;
  GdiTrack(dc, &g_capture, s->Width(), s->Height(), "hud text capture");
  return true;
}

void HudTextAfterDraw(Surface* rt, Surface* tex, const uint8_t* verts, DWORD first, DWORD count, UINT stride,
                      int uv_off) {
  if (g_panels.empty() || !rt || !verts || count < 3 || !Active()) return;
  const float* fv = reinterpret_cast<const float*>(verts + static_cast<size_t>(first) * stride);
  auto V = [&](DWORD i) { return reinterpret_cast<const float*>(verts + static_cast<size_t>(first + i) * stride); };
  DWORD ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;
  for (DWORD i = 1; i < count; ++i) {
    if (V(i)[0] < V(ix0)[0]) ix0 = i;
    if (V(i)[0] > V(ix1)[0]) ix1 = i;
    if (V(i)[1] < V(iy0)[1]) iy0 = i;
    if (V(i)[1] > V(iy1)[1]) iy1 = i;
  }
  (void)fv;
  const float x0 = V(ix0)[0], x1 = V(ix1)[0], y0 = V(iy0)[1], y1 = V(iy1)[1];
  // The panel's own quad.
  if (auto* e = QuadOf(x0, y0, x1, y1)) {
    ++g_st.draws;
    ++e->second.draws;
    if (!e->second.items.empty()) DrawPanel(rt, e->first, e->second);
    return;
  }
  // A piece of a panel (or of a copy of one) drawn somewhere else: the name
  // labels over the riders.
  if (!tex || uv_off <= 0) return;
  const float tw = static_cast<float>(tex->Width()), th = static_cast<float>(tex->Height());
  auto UV = [&](DWORD i) { return reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(V(i)) + uv_off); };
  const float u0 = UV(ix0)[0] * tw, u1 = UV(ix1)[0] * tw, v0 = UV(iy0)[1] * th, v1 = UV(iy1)[1] * th;
  if (std::fabs(u1 - u0) < 1 || std::fabs(v1 - v0) < 1) return;
  const RECT texels{static_cast<LONG>(std::floor(std::min(u0, u1))), static_cast<LONG>(std::floor(std::min(v0, v1))),
                    static_cast<LONG>(std::ceil(std::max(u0, u1))), static_cast<LONG>(std::ceil(std::max(v0, v1)))};
  uintptr_t self = 0;
  const Copy* c = nullptr;
  if (!FindSource(tex, texels, &self, &c)) return;
  auto pit = g_panels.find(self);
  if (pit == g_panels.end()) return;
  ++g_st.labels;
  // Screen = sx0 + (texel - u0) * kx; texel = dr.left + (panel - sr.left) * cs.
  const float kx = (x1 - x0) / (u1 - u0), ky = (y1 - y0) / (v1 - v0);
  float csx = 1, csy = 1, tx = 0, ty = 0;  // texel = tx + panel * cs
  if (c) {
    csx = static_cast<float>(c->dr.right - c->dr.left) / std::max<LONG>(1, c->sr.right - c->sr.left);
    csy = static_cast<float>(c->dr.bottom - c->dr.top) / std::max<LONG>(1, c->sr.bottom - c->sr.top);
    tx = c->dr.left - c->sr.left * csx;
    ty = c->dr.top - c->sr.top * csy;
  }
  const float ax = x0 + (tx - u0) * kx, ay = y0 + (ty - v0) * ky;
  const RECT region{static_cast<LONG>(std::floor((texels.left - tx) / csx)),
                    static_cast<LONG>(std::floor((texels.top - ty) / csy)),
                    static_cast<LONG>(std::ceil((texels.right - tx) / csx)),
                    static_cast<LONG>(std::ceil((texels.bottom - ty) / csy))};
  const RECT clip{static_cast<LONG>(std::floor(x0)), static_cast<LONG>(std::floor(y0)),
                  static_cast<LONG>(std::ceil(x1)), static_cast<LONG>(std::ceil(y1))};
  static unsigned logged = 0;
  if (++logged <= 12)
    Log("hud text: label draw, %lu vertices, texture %ux%u texels %ld,%ld-%ld,%ld, screen %.1f,%.1f-%.1f,%.1f",
        static_cast<unsigned long>(count), tex->Width(), tex->Height(), texels.left, texels.top, texels.right,
        texels.bottom, x0, y0, x1, y1);
  DrawItems(rt, self, pit->second, region, ax, ay, kx * csx, ky * csy, clip, "label");
}

float HudTextLabelScale(Surface* tex, const uint8_t* verts, DWORD first, DWORD count, UINT stride) {
  if (!tex || !verts || count < 3 || g_copies.empty() || !Active() || !g_copies.count(tex)) return 1.0f;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (DWORD i = 0; i < count; ++i) {
    const float* p = reinterpret_cast<const float*>(verts + static_cast<size_t>(first + i) * stride);
    x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]);
    y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
  }
  // A panel's own quad is laid out larger already.
  if (QuadOf(x0, y0, x1, y1)) return 1.0f;
  return Scale();
}

void HudTextBlt(Surface* src, const RECT& sr, Surface* dst, const RECT& dr) {
  if (!src || !dst || src == dst || g_panel_tex.empty()) return;
  auto pt = g_panel_tex.find(src);
  if (pt == g_panel_tex.end()) return;
  ++g_st.copies;
  auto& v = g_copies[dst];
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Copy& c) { return Intersects(c.dr, dr); }), v.end());
  v.push_back(Copy{pt->second, sr, dr});
  static unsigned logged = 0;
  if (++logged <= 10)
    Log("hud text: panel %08x pixels %ld,%ld-%ld,%ld copied to a %ux%u texture at %ld,%ld-%ld,%ld",
        static_cast<unsigned>(pt->second), sr.left, sr.top, sr.right, sr.bottom, dst->Width(), dst->Height(), dr.left,
        dr.top, dr.right, dr.bottom);
}

void HudTextForget(Surface* s) {
  g_copies.erase(s);
  g_panel_tex.erase(s);
  g_scratch.erase(s);
  g_pending.erase(s);
}

void HudPanelLayout(uintptr_t inst) {
  if (!Active() || Scale() == 1.0f) return;
  auto it = g_panels.find(inst);
  if (it != g_panels.end()) ScaleRect(inst, it->second);
}

void InstallHudTextHooks() {
  if (!GetConfig().gpu_text || !GetConfig().hud_text) return;
  unsigned ok = 0, total = 0;
  for (uintptr_t site : kConvertCalls) {
    ++total;
    ok += PatchCall(site, kConvert, reinterpret_cast<const void*>(&ConvertHook));
  }
  for (uintptr_t site : kRestoreCalls) {
    ++total;
    ok += PatchCall(site, kRestore, reinterpret_cast<const void*>(&RestoreHook));
  }
  Log("hud text: %u of %u HUD panel calls hooked; panels drawn x%.2f", ok, total, Scale());
}

}  // namespace dd
