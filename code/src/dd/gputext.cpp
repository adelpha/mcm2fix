#include "gputext.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../config.h"
#include "../patch.h"
#include "gdihook.h"

namespace dd {
namespace {

// ------------------------------------------------------------------ logging

unsigned g_log_lines = 0;
constexpr unsigned kMaxLogLines = 800;

void TLog(const char* fmt, ...) {
  if (!GetConfig().text_log || g_log_lines >= kMaxLogLines) return;
  ++g_log_lines;
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Log("text: %s", buf);
}

struct TextStats {
  unsigned dcs = 0, calls = 0, glyphs = 0, fills = 0, new_glyphs = 0, uploads = 0, occlusions = 0, composites = 0,
           fallbacks = 0, copies = 0;
} g_ts;

void Report() {
  static DWORD last = GetTickCount();
  const DWORD now = GetTickCount();
  if (now - last < 5000) return;
  last = now;
  if ((GetConfig().text_log || GetConfig().dd_stats) && (g_ts.dcs || g_ts.composites))
    Log("gpu text 5s: %u DCs, %u calls, %u glyphs, %u fills, %u new glyphs, %u atlas uploads, %u erased, "
        "%u composites, %u copies, %u not drawn",
        g_ts.dcs, g_ts.calls, g_ts.glyphs, g_ts.fills, g_ts.new_glyphs, g_ts.uploads, g_ts.occlusions,
        g_ts.composites, g_ts.copies, g_ts.fallbacks);
  g_ts = TextStats{};
}

// ------------------------------------------------------------------ overlays

struct Overlay {
  GpuTexture tex;
  UINT base_w = 0, base_h = 0;
  float sx = 1, sy = 1;  // overlay pixels per surface pixel
  bool empty = true;
};

std::map<const void*, Overlay> g_overlays;
unsigned g_nonempty = 0;

void MarkUsed(Overlay& o) {
  if (o.empty) {
    o.empty = false;
    ++g_nonempty;
  }
}

void MarkEmpty(Overlay& o) {
  if (!o.empty) {
    o.empty = true;
    --g_nonempty;
  }
}

// The size the surface is shown at: fitted into the window, keeping its
// aspect ratio, never smaller than the surface.
void FitSize(UINT bw, UINT bh, UINT* ow, UINT* oh) {
  RECT rc{};
  double s = 1.0;
  if (B().hwnd && GetClientRect(B().hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
    s = std::min(static_cast<double>(rc.right) / bw, static_cast<double>(rc.bottom) / bh);
  }
  s = std::max(1.0, std::min(s, 4.0));
  *ow = std::min<UINT>(static_cast<UINT>(bw * s + 0.5), 4096);
  *oh = std::min<UINT>(static_cast<UINT>(bh * s + 0.5), 4096);
}

void ClearOverlay(Overlay& o, const RECT* r) {
  if (o.empty || !o.tex.Valid()) return;
  BlitParams p;
  p.SetDst(o.tex);
  if (r) {
    // Rounded the same way as copies (ScaleRect), so an erase followed by a
    // copy of the same rectangle leaves no seams.
    p.dst_rect = {static_cast<LONG>(std::lround(r->left * o.sx)), static_cast<LONG>(std::lround(r->top * o.sy)),
                  static_cast<LONG>(std::lround(r->right * o.sx)), static_cast<LONG>(std::lround(r->bottom * o.sy))};
    p.dst_rect.left = std::max<LONG>(p.dst_rect.left, 0);
    p.dst_rect.top = std::max<LONG>(p.dst_rect.top, 0);
    p.dst_rect.right = std::min<LONG>(p.dst_rect.right, static_cast<LONG>(o.tex.width));
    p.dst_rect.bottom = std::min<LONG>(p.dst_rect.bottom, static_cast<LONG>(o.tex.height));
    if (p.dst_rect.left >= p.dst_rect.right || p.dst_rect.top >= p.dst_rect.bottom) return;
  } else {
    p.dst_rect = {0, 0, static_cast<LONG>(o.tex.width), static_cast<LONG>(o.tex.height)};
  }
  p.fill[0] = p.fill[1] = p.fill[2] = p.fill[3] = 0;
  p.opaque = false;
  Blit(p);
  if (!r || (p.dst_rect.left == 0 && p.dst_rect.top == 0 && p.dst_rect.right == static_cast<LONG>(o.tex.width) &&
             p.dst_rect.bottom == static_cast<LONG>(o.tex.height)))
    MarkEmpty(o);
}

// Overlay pixels per surface pixel for textures the game shows on screen at
// about its 640x480 layout size (HUD panels): the screen's scale over that.
float TextureScale() {
  RECT rc{};
  double s = 1.0;
  if (B().hwnd && GetClientRect(B().hwnd, &rc) && rc.right > 0 && rc.bottom > 0)
    s = std::min(rc.right / 640.0, rc.bottom / 480.0);
  return static_cast<float>(std::max(1.0, std::min(s, 4.0)));
}

// scale 0: fitted to the window (FitSize).
Overlay* EnsureOverlay(const void* native, UINT bw, UINT bh, float scale = 0) {
  UINT ow = 0, oh = 0;
  if (scale > 0) {
    ow = std::min<UINT>(static_cast<UINT>(bw * scale + 0.5f), 4096);
    oh = std::min<UINT>(static_cast<UINT>(bh * scale + 0.5f), 4096);
  } else {
    FitSize(bw, bh, &ow, &oh);
  }
  Overlay& o = g_overlays[native];
  if (o.tex.Valid() && o.tex.width == ow && o.tex.height == oh && o.base_w == bw && o.base_h == bh) return &o;
  MarkEmpty(o);
  GpuTexture old = o.tex;  // reset below, outside the map's bookkeeping
  o.tex = GpuTexture{};
  old.Reset();
  if (!CreateGpuTexture(o.tex, ow, oh, 1, true, false, true)) {
    Log("text overlay %ux%u could not be made; GDI text is not drawn", ow, oh);
    g_overlays.erase(native);
    return nullptr;
  }
  o.base_w = bw;
  o.base_h = bh;
  o.sx = static_cast<float>(ow) / bw;
  o.sy = static_cast<float>(oh) / bh;
  o.empty = false;
  ++g_nonempty;
  ClearOverlay(o, nullptr);
  Log("text overlay %ux%u for a %ux%u surface (x%.2f)", ow, oh, bw, bh, o.sx);
  return &o;
}

// -------------------------------------------------------------- glyph atlas

constexpr UINT kAtlas = 1024;

struct Glyph {
  int ax = 0, ay = 0;  // atlas position
  int bw = 0, bh = 0;  // black box
  int ox = 0, oy = 0;  // black box origin relative to the pen (y up)
  float adv = 0;
};

struct HiFont {
  HFONT font = nullptr;
  int ascent = 0;
  std::map<unsigned char, Glyph> glyphs;
};

std::map<std::string, std::unique_ptr<HiFont>> g_fonts;
std::vector<uint8_t> g_atlas_px;
GpuTexture g_atlas;
bool g_atlas_dirty = false;
int g_shelf_x = 1, g_shelf_y = 1, g_shelf_h = 0;
unsigned g_atlas_resets = 0;
HDC g_glyph_dc = nullptr;

void ResetAtlas() {
  for (auto& kv : g_fonts) kv.second->glyphs.clear();
  std::fill(g_atlas_px.begin(), g_atlas_px.end(), 0);
  g_shelf_x = g_shelf_y = 1;
  g_shelf_h = 0;
  g_atlas_dirty = true;
  if (++g_atlas_resets <= 5) Log("glyph atlas full; started again (%u)", g_atlas_resets);
}

HiFont* GetFont(HDC dc, float scale) {
  LOGFONTA lf{};
  HGDIOBJ cur = GetCurrentObject(dc, OBJ_FONT);
  if (!cur || !GetObjectA(cur, sizeof(lf), &lf)) return nullptr;
  char key[160];
  std::snprintf(key, sizeof(key), "%s|%ld|%ld|%ld|%ld|%d|%d|%d|%d|%d", lf.lfFaceName, lf.lfHeight, lf.lfWidth,
                lf.lfEscapement, lf.lfWeight, lf.lfItalic, lf.lfUnderline, lf.lfStrikeOut, lf.lfCharSet,
                static_cast<int>(scale * 100 + 0.5f));
  auto& slot = g_fonts[key];
  if (slot) return slot.get();
  if (!g_glyph_dc) g_glyph_dc = CreateCompatibleDC(nullptr);
  LOGFONTA hi = lf;
  hi.lfHeight = static_cast<LONG>(std::lround(lf.lfHeight * scale));
  hi.lfWidth = static_cast<LONG>(std::lround(lf.lfWidth * scale));
  if (lf.lfHeight == 0) hi.lfHeight = static_cast<LONG>(std::lround(-12 * scale));
  hi.lfQuality = ANTIALIASED_QUALITY;
  auto f = std::make_unique<HiFont>();
  f->font = CreateFontIndirectA(&hi);
  if (!f->font) return nullptr;
  HGDIOBJ old = SelectObject(g_glyph_dc, f->font);
  TEXTMETRICA tm{};
  GetTextMetricsA(g_glyph_dc, &tm);
  SelectObject(g_glyph_dc, old);
  const char* sub = "";
  if (!(tm.tmPitchAndFamily & (TMPF_TRUETYPE | TMPF_VECTOR))) {
    // A bitmap font (System, Small Fonts) has no outlines to draw smoothly
    // at another size: use Arial instead.
    DeleteObject(f->font);
    std::strcpy(hi.lfFaceName, "Arial");
    hi.lfCharSet = ANSI_CHARSET;
    f->font = CreateFontIndirectA(&hi);
    if (!f->font) return nullptr;
    old = SelectObject(g_glyph_dc, f->font);
    GetTextMetricsA(g_glyph_dc, &tm);
    SelectObject(g_glyph_dc, old);
    sub = " (bitmap font: Arial instead)";
  }
  f->ascent = tm.tmAscent;
  Log("text font \"%s\" %ld (weight %ld%s) drawn at %ld for x%.2f%s", lf.lfFaceName, lf.lfHeight, lf.lfWeight,
      lf.lfItalic ? " italic" : "", hi.lfHeight, scale, sub);
  slot = std::move(f);
  return slot.get();
}

// Returns null when the atlas is full (the caller starts it again).
const Glyph* GetGlyph(HiFont& f, unsigned char ch) {
  auto it = f.glyphs.find(ch);
  if (it != f.glyphs.end()) return &it->second;
  if (g_atlas_px.empty()) g_atlas_px.assign(static_cast<size_t>(kAtlas) * kAtlas * 4, 0);
  HGDIOBJ old = SelectObject(g_glyph_dc, f.font);
  const MAT2 m{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
  GLYPHMETRICS gm{};
  Glyph g;
  DWORD size = GetGlyphOutlineA(g_glyph_dc, ch, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
  std::vector<uint8_t> buf;
  if (size != GDI_ERROR && size > 0) {
    buf.resize(size);
    if (GetGlyphOutlineA(g_glyph_dc, ch, GGO_GRAY8_BITMAP, &gm, size, buf.data(), &m) == GDI_ERROR) buf.clear();
  } else if (size == GDI_ERROR) {
    // No outline (bitmap font): advance only.
    SIZE sz{};
    char c = static_cast<char>(ch);
    GetTextExtentPoint32A(g_glyph_dc, &c, 1, &sz);
    gm = GLYPHMETRICS{};
    gm.gmCellIncX = static_cast<short>(sz.cx);
  }
  SelectObject(g_glyph_dc, old);
  g.adv = gm.gmCellIncX;
  if (!buf.empty()) {
    g.bw = static_cast<int>(gm.gmBlackBoxX);
    g.bh = static_cast<int>(gm.gmBlackBoxY);
    g.ox = gm.gmptGlyphOrigin.x;
    g.oy = gm.gmptGlyphOrigin.y;
    if (g_shelf_x + g.bw + 1 > static_cast<int>(kAtlas)) {
      g_shelf_x = 1;
      g_shelf_y += g_shelf_h + 1;
      g_shelf_h = 0;
    }
    if (g_shelf_y + g.bh + 1 > static_cast<int>(kAtlas)) return nullptr;
    g.ax = g_shelf_x;
    g.ay = g_shelf_y;
    g_shelf_x += g.bw + 1;
    g_shelf_h = std::max(g_shelf_h, g.bh);
    const UINT pitch = (gm.gmBlackBoxX + 3) & ~3u;
    for (int y = 0; y < g.bh; ++y)
      for (int x = 0; x < g.bw; ++x) {
        const unsigned v = std::min<unsigned>(buf[static_cast<size_t>(y) * pitch + x], 64);
        const uint8_t a = static_cast<uint8_t>((v * 255 + 32) / 64);
        uint8_t* px = &g_atlas_px[(static_cast<size_t>(g.ay + y) * kAtlas + g.ax + x) * 4];
        px[0] = px[1] = px[2] = px[3] = a;  // white, premultiplied
      }
    g_atlas_dirty = true;
  }
  ++g_ts.new_glyphs;
  return &(f.glyphs[ch] = g);
}

bool UploadAtlas() {
  if (!g_atlas_dirty) return g_atlas.Valid();
  g_atlas_dirty = false;
  ++g_ts.uploads;
  if (!g_atlas.Valid()) {
    if (!CreateTextureFromBGRA(g_atlas, g_atlas_px.data(), kAtlas, kAtlas)) {
      Log("glyph atlas texture could not be made");
      return false;
    }
    return true;
  }
  UploadLevel(g_atlas, 0, g_atlas_px.data(), kAtlas, kAtlas);
  return true;
}

// ------------------------------------------------------------------ drawing

struct Ctx {
  Overlay* o;
  std::vector<RECT> clips;  // overlay pixels
};

RECT ScaleRect(const Overlay& o, const RECT& r) {
  return RECT{static_cast<LONG>(std::lround(r.left * o.sx)), static_cast<LONG>(std::lround(r.top * o.sy)),
              static_cast<LONG>(std::lround(r.right * o.sx)), static_cast<LONG>(std::lround(r.bottom * o.sy))};
}

// The DC's clip region (surface pixels), as overlay rectangles.
void ReadClip(HDC dc, Ctx& c, const RECT* extra) {
  const Overlay& o = *c.o;
  RECT full{0, 0, static_cast<LONG>(o.base_w), static_cast<LONG>(o.base_h)};
  if (extra) IntersectRect(&full, &full, extra);
  std::vector<RECT> base;
  HRGN rgn = CreateRectRgn(0, 0, 0, 0);
  if (GetClipRgn(dc, rgn) == 1) {
    const DWORD n = GetRegionData(rgn, 0, nullptr);
    std::vector<uint8_t> data(n);
    if (n && GetRegionData(rgn, n, reinterpret_cast<RGNDATA*>(data.data()))) {
      auto* rd = reinterpret_cast<RGNDATA*>(data.data());
      auto* rs = reinterpret_cast<RECT*>(rd->Buffer);
      for (DWORD i = 0; i < rd->rdh.nCount; ++i) {
        RECT r;
        if (IntersectRect(&r, &rs[i], &full)) base.push_back(r);
      }
    }
  } else {
    base.push_back(full);
  }
  DeleteObject(rgn);
  c.clips.clear();
  for (const RECT& r : base) {
    RECT s = ScaleRect(o, r);
    if (s.left < s.right && s.top < s.bottom) c.clips.push_back(s);
  }
}

void Fill(Ctx& c, const RECT& base, COLORREF col) {
  const RECT s = ScaleRect(*c.o, base);
  for (const RECT& clip : c.clips) {
    RECT r;
    if (!IntersectRect(&r, &s, &clip)) continue;
    BlitParams p;
    p.SetDst(c.o->tex);
    p.dst_rect = r;
    p.fill[0] = GetRValue(col) / 255.0f;
    p.fill[1] = GetGValue(col) / 255.0f;
    p.fill[2] = GetBValue(col) / 255.0f;
    p.fill[3] = 1.0f;
    p.opaque = false;
    Blit(p);
    ++g_ts.fills;
  }
  MarkUsed(*c.o);
}

void DrawGlyph(Ctx& c, const Glyph& g, int x, int y, COLORREF col) {
  const RECT q{x, y, x + g.bw, y + g.bh};
  for (const RECT& clip : c.clips) {
    RECT r;
    if (!IntersectRect(&r, &q, &clip)) continue;
    BlitParams p;
    p.SetDst(c.o->tex);
    p.dst_rect = r;
    p.SetSrc(g_atlas);
    p.src_rect = {g.ax + (r.left - q.left), g.ay + (r.top - q.top), g.ax + (r.right - q.left),
                  g.ay + (r.bottom - q.top)};
    p.fill[0] = GetRValue(col) / 255.0f;
    p.fill[1] = GetGValue(col) / 255.0f;
    p.fill[2] = GetBValue(col) / 255.0f;
    p.fill[3] = 1.0f;
    p.tint = true;
    p.premul = true;
    p.opaque = false;
    Blit(p);
    ++g_ts.glyphs;
  }
}

// One line of text with its top-left corner at (left, top), surface pixels.
void DrawLine(Ctx& c, HDC dc, int left, int top, const char* s, int n) {
  if (n <= 0) return;
  SIZE ext{};
  GetTextExtentPoint32A(dc, s, n, &ext);
  TEXTMETRICA tm{};
  GetTextMetricsA(dc, &tm);
  if (GetBkMode(dc) == OPAQUE) Fill(c, RECT{left, top, left + ext.cx, top + ext.cy}, GetBkColor(dc));
  Overlay& o = *c.o;
  HiFont* f = GetFont(dc, o.sy);
  if (!f) return;
  std::vector<const Glyph*> gl(static_cast<size_t>(n));
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool ok = true;
    for (int i = 0; i < n && ok; ++i) ok = (gl[i] = GetGlyph(*f, static_cast<unsigned char>(s[i]))) != nullptr;
    if (ok) break;
    ResetAtlas();
    if (attempt == 1) return;
  }
  if (!UploadAtlas()) return;
  const float extra = static_cast<float>(GetTextCharacterExtra(dc)) * o.sx;
  float sum = 0;
  for (const Glyph* g : gl) sum += g->adv + extra;
  // Keep the game's layout: stretch the advances to the width GDI measured.
  float k = sum > 0 ? ext.cx * o.sx / sum : 1.0f;
  k = std::max(0.85f, std::min(k, 1.15f));
  const COLORREF col = GetTextColor(dc);
  float pen = left * o.sx;
  const float baseline = (top + tm.tmAscent) * o.sy;
  for (const Glyph* g : gl) {
    if (g->bw > 0)
      DrawGlyph(c, *g, static_cast<int>(std::lround(pen + g->ox)), static_cast<int>(std::lround(baseline - g->oy)),
                col);
    pen += (g->adv + extra) * k;
  }
  MarkUsed(o);
}

COLORREF BrushColor(HBRUSH br) {
  const UINT_PTR v = reinterpret_cast<UINT_PTR>(br);
  if (v > 0 && v <= 31) return GetSysColor(static_cast<int>(v - 1));
  LOGBRUSH lb{};
  if (br && GetObjectA(br, sizeof(lb), &lb)) {
    if (lb.lbStyle != BS_SOLID) LogOnce("GPU text: brush style %u drawn as solid", lb.lbStyle);
    return lb.lbColor;
  }
  return RGB(0, 0, 0);
}

int TextWidth(HDC dc, const std::string& s) {
  SIZE sz{};
  if (!s.empty()) GetTextExtentPoint32A(dc, s.data(), static_cast<int>(s.size()), &sz);
  return sz.cx;
}

void DoDrawText(Ctx& c, HDC dc, LPCSTR str, int len, const RECT& rc, UINT fmt) {
  if (!str) return;
  if (len < 0) len = static_cast<int>(std::strlen(str));
  // Prefix characters: "&x" underlines x (not drawn), "&&" is "&".
  std::string text;
  for (int i = 0; i < len; ++i) {
    char ch = str[i];
    if (ch == '&' && !(fmt & DT_NOPREFIX)) {
      if (i + 1 < len && str[i + 1] == '&') {
        text += '&';
        ++i;
      }
      continue;
    }
    text += ch;
  }
  const int width = rc.right - rc.left;
  std::vector<std::string> lines;
  if (fmt & DT_SINGLELINE) {
    std::string one;
    for (char ch : text) one += (ch == '\r' || ch == '\n') ? ' ' : ch;
    lines.push_back(one);
  } else {
    std::string cur;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        if (fmt & DT_WORDBREAK) {
          // Greedy word wrap with GDI's widths.
          std::string line;
          size_t p = 0;
          while (p < cur.size()) {
            size_t e = cur.find(' ', p);
            if (e == std::string::npos) e = cur.size();
            std::string word = cur.substr(p, e - p);
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && TextWidth(dc, trial) > width) {
              lines.push_back(line);
              line = word;
            } else {
              line = trial;
            }
            p = e + 1;
          }
          lines.push_back(line);
        } else {
          lines.push_back(cur);
        }
        cur.clear();
      } else {
        cur += text[i];
      }
    }
  }
  if (fmt & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) {
    for (std::string& l : lines) {
      if (TextWidth(dc, l) <= width) continue;
      while (!l.empty() && TextWidth(dc, l + "...") > width) l.pop_back();
      l += "...";
    }
  }
  TEXTMETRICA tm{};
  GetTextMetricsA(dc, &tm);
  const int lh = tm.tmHeight + ((fmt & DT_EXTERNALLEADING) ? tm.tmExternalLeading : 0);
  int y = rc.top;
  if (fmt & DT_SINGLELINE) {
    if (fmt & DT_VCENTER) y = rc.top + (rc.bottom - rc.top - lh) / 2;
    else if (fmt & DT_BOTTOM) y = rc.bottom - lh;
  }
  for (const std::string& l : lines) {
    const int w = TextWidth(dc, l);
    int x = rc.left;
    if (fmt & DT_CENTER) x = rc.left + (width - w) / 2;
    else if (fmt & DT_RIGHT) x = rc.right - w;
    DrawLine(c, dc, x, y, l.data(), static_cast<int>(l.size()));
    y += lh;
  }
}

// ----------------------------------------------------------- recording DCs

class Recorder : public GdiTarget {
 public:
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  UINT w = 0, h = 0;
  bool busy = false;
  const void* native = nullptr;
  // Record only: GDI still draws on the DC (a texture's CPU copy); the text
  // is also drawn into an overlay at `scale`, for hudtext.cpp to use.
  bool record_only = false;
  float scale = 0;

  bool Draw(const GdiCall& call) override {
    const bool drawn = Record(call);
    return drawn && !record_only;
  }

  bool Record(const GdiCall& call) {
    ++g_ts.calls;
    Overlay* o = EnsureOverlay(native, w, h, scale);
    if (!o) {
      ++g_ts.fallbacks;
      return false;
    }
    Ctx c{o, {}};
    switch (call.fn) {
      case GdiFn::kTextOut: {
        if (!call.s || call.len <= 0) return true;
        SIZE ext{};
        GetTextExtentPoint32A(dc, call.s, call.len, &ext);
        TEXTMETRICA tm{};
        GetTextMetricsA(dc, &tm);
        const UINT align = GetTextAlign(dc);
        int x = call.x, y = call.y;
        if (align & TA_UPDATECP) {
          POINT p{};
          GetCurrentPositionEx(dc, &p);
          x = p.x;
          y = p.y;
        }
        int left = x, top = y;
        if ((align & TA_CENTER) == TA_CENTER) left -= ext.cx / 2;
        else if (align & TA_RIGHT) left -= ext.cx;
        if ((align & TA_BASELINE) == TA_BASELINE) top -= tm.tmAscent;
        else if (align & TA_BOTTOM) top -= ext.cy;
        if ((align & TA_UPDATECP) && !record_only)
          MoveToEx(dc, (align & TA_RIGHT) ? x - ext.cx : x + ext.cx, y, nullptr);
        ReadClip(dc, c, nullptr);
        DrawLine(c, dc, left, top, call.s, call.len);
        TLog("TextOut %d,%d \"%.*s\"", left, top, std::min(call.len, 40), call.s);
        return true;
      }
      case GdiFn::kDrawText: {
        ReadClip(dc, c, (call.fmt & DT_NOCLIP) ? nullptr : call.rect);
        DoDrawText(c, dc, call.s, call.len, *call.rect, call.fmt);
        TLog("DrawText %ld,%ld-%ld,%ld fmt %x", call.rect->left, call.rect->top, call.rect->right,
             call.rect->bottom, call.fmt);
        return true;
      }
      case GdiFn::kFillRect: {
        ReadClip(dc, c, nullptr);
        Fill(c, *call.rect, BrushColor(call.brush));
        TLog("FillRect %ld,%ld-%ld,%ld", call.rect->left, call.rect->top, call.rect->right, call.rect->bottom);
        return true;
      }
      case GdiFn::kFrameRect: {
        ReadClip(dc, c, nullptr);
        const RECT& r = *call.rect;
        const COLORREF col = BrushColor(call.brush);
        if (r.right <= r.left || r.bottom <= r.top) return true;
        Fill(c, RECT{r.left, r.top, r.right, r.top + 1}, col);
        Fill(c, RECT{r.left, r.bottom - 1, r.right, r.bottom}, col);
        Fill(c, RECT{r.left, r.top + 1, r.left + 1, r.bottom - 1}, col);
        Fill(c, RECT{r.right - 1, r.top + 1, r.right, r.bottom - 1}, col);
        TLog("FrameRect %ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
        return true;
      }
      case GdiFn::kLineTo: {
        ReadClip(dc, c, nullptr);
        POINT p{};
        GetCurrentPositionEx(dc, &p);
        LOGPEN lp{};
        HGDIOBJ pen = GetCurrentObject(dc, OBJ_PEN);
        if (pen) GetObjectA(pen, sizeof(lp), &lp);
        if (lp.lopnStyle == PS_NULL) return true;
        const int wpx = std::max<int>(1, lp.lopnWidth.x);
        // GDI leaves out the end point.
        const int dx = call.x - p.x, dy = call.y - p.y;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        if (dx == 0 && dy == 0) return true;
        if (dy == 0) {
          const LONG x0 = dx > 0 ? p.x : call.x + 1, x1 = dx > 0 ? call.x : p.x + 1;
          Fill(c, RECT{x0, p.y, x1, p.y + wpx}, lp.lopnColor);
        } else if (dx == 0) {
          const LONG y0 = dy > 0 ? p.y : call.y + 1, y1 = dy > 0 ? call.y : p.y + 1;
          Fill(c, RECT{p.x, y0, p.x + wpx, y1}, lp.lopnColor);
        } else {
          for (int i = 0; i < steps; ++i) {
            const int x = p.x + dx * i / steps, y = p.y + dy * i / steps;
            Fill(c, RECT{x, y, x + wpx, y + wpx}, lp.lopnColor);
          }
        }
        TLog("LineTo %ld,%ld -> %d,%d", p.x, p.y, call.x, call.y);
        return true;
      }
    }
    return false;
  }
};

std::vector<std::unique_ptr<Recorder>> g_recorders;

}  // namespace

bool GpuTextActive() { return GetConfig().gpu_text && Ready(); }

HDC GpuTextBeginDC(const GpuTexture& base, UINT w, UINT h) {
  if (!GpuTextActive() || !base.Valid() || !w || !h) return nullptr;
  Report();
  Recorder* r = nullptr;
  for (auto& rec : g_recorders)
    if (!rec->busy && !rec->record_only && rec->w == w && rec->h == h) {
      r = rec.get();
      break;
    }
  if (!r) {
    auto rec = std::make_unique<Recorder>();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    rec->bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    rec->dc = CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!rec->bmp || !rec->dc) {
      if (rec->dc) DeleteDC(rec->dc);
      if (rec->bmp) DeleteObject(rec->bmp);
      LogOnce("GPU text: recording DC could not be made");
      return nullptr;
    }
    rec->old = SelectObject(rec->dc, rec->bmp);
    rec->w = w;
    rec->h = h;
    r = rec.get();
    g_recorders.push_back(std::move(rec));
  }
  HDC dc = r->dc;
  // A fresh DC state each time, like a new surface DC.
  SelectClipRgn(dc, nullptr);
  SetTextColor(dc, RGB(0, 0, 0));
  SetBkColor(dc, RGB(255, 255, 255));
  SetBkMode(dc, OPAQUE);
  SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
  SetTextCharacterExtra(dc, 0);
  SelectObject(dc, GetStockObject(SYSTEM_FONT));
  SelectObject(dc, GetStockObject(BLACK_PEN));
  SelectObject(dc, GetStockObject(WHITE_BRUSH));
  MoveToEx(dc, 0, 0, nullptr);
  r->busy = true;
  r->native = base.Native();
  GdiTrack(dc, r, w, h, "gpu text");
  ++g_ts.dcs;
  LogOnce("GPU text: the game's GDI drawing is drawn by mcm2fix (GpuText=1)");
  return dc;
}

bool GpuTextRecordDC(HDC dc, const void* key, UINT w, UINT h) {
  if (!GpuTextActive() || !dc || !key || !w || !h) return false;
  Recorder* r = nullptr;
  for (auto& rec : g_recorders)
    if (!rec->busy && rec->record_only) {
      r = rec.get();
      break;
    }
  if (!r) {
    g_recorders.push_back(std::make_unique<Recorder>());
    r = g_recorders.back().get();
    r->record_only = true;
  }
  r->dc = dc;
  r->w = w;
  r->h = h;
  r->native = key;
  r->scale = GpuTextTextureScale();
  r->busy = true;
  GdiTrack(dc, r, w, h, "cpu copy + text recording");
  ++g_ts.dcs;
  return true;
}

float g_texture_scale = 0;  // set from what the game's draws show (hudtext.cpp)
float GpuTextTextureScale() { return g_texture_scale > 0 ? g_texture_scale : TextureScale(); }
void GpuTextSetTextureScale(float s) { g_texture_scale = s; }

bool GpuTextBake(const void* key, const RECT& r, const GpuTexture& dst, float dst_sx, float dst_sy,
                 const float rgba[4]) {
  if (!g_nonempty || !key || !dst.CanRender()) return false;
  auto it = g_overlays.find(key);
  if (it == g_overlays.end() || it->second.empty || !it->second.tex.Valid()) return false;
  const Overlay& o = it->second;
  BlitParams p;
  p.SetDst(dst);
  p.SetSrc(o.tex);
  p.src_rect = ScaleRect(o, r);
  // The destination is a copy of a surface of its own size, dst_sx/dst_sy
  // copy pixels per surface pixel; surface pixel (x,y) of the source is
  // surface pixel (x,y) there too.
  const float kx = dst_sx / o.sx, ky = dst_sy / o.sy;
  p.dst_rect = {static_cast<LONG>(std::lround(p.src_rect.left * kx)), static_cast<LONG>(std::lround(p.src_rect.top * ky)),
                static_cast<LONG>(std::lround(p.src_rect.right * kx)),
                static_cast<LONG>(std::lround(p.src_rect.bottom * ky))};
  p.src_rect.left = std::max<LONG>(p.src_rect.left, 0);
  p.src_rect.top = std::max<LONG>(p.src_rect.top, 0);
  p.src_rect.right = std::min<LONG>(p.src_rect.right, static_cast<LONG>(o.tex.width));
  p.src_rect.bottom = std::min<LONG>(p.src_rect.bottom, static_cast<LONG>(o.tex.height));
  if (p.src_rect.left >= p.src_rect.right || p.src_rect.top >= p.src_rect.bottom) return false;
  p.linear = std::fabs(kx - 1) > 1e-4f || std::fabs(ky - 1) > 1e-4f;
  // Premultiplied tint: the overlay is white text with coverage in alpha.
  p.fill[0] = rgba[0] * rgba[3];
  p.fill[1] = rgba[1] * rgba[3];
  p.fill[2] = rgba[2] * rgba[3];
  p.fill[3] = rgba[3];
  p.tint = true;
  p.premul = true;
  p.opaque = false;
  Blit(p);
  ++g_ts.composites;
  TLog("baked text %ld,%ld-%ld,%ld into a %ux%u copy", r.left, r.top, r.right, r.bottom, dst.width, dst.height);
  return true;
}

void GpuTextEndDC(HDC dc) {
  GdiUntrack(dc);
  for (auto& rec : g_recorders)
    if (rec->busy && rec->dc == dc) rec->busy = false;
}

void GpuTextOcclude(const void* native, const RECT* r, const char* why) {
  if (!g_nonempty || !native) return;
  auto it = g_overlays.find(native);
  if (it == g_overlays.end() || it->second.empty) return;
  ++g_ts.occlusions;
  if (r)
    TLog("erased %ld,%ld-%ld,%ld (%s)", r->left, r->top, r->right, r->bottom, why);
  else
    TLog("erased all (%s)", why);
  ClearOverlay(it->second, r);
}

void GpuTextOccludeSprite(const void* native, const BlitParams& sprite) {
  if (!g_nonempty || !native) return;
  auto it = g_overlays.find(native);
  if (it == g_overlays.end() || it->second.empty) return;
  Overlay& o = it->second;
  ++g_ts.occlusions;
  const RECT& d = sprite.dst_rect;
  TLog("erased sprite shape %ld,%ld-%ld,%ld", d.left, d.top, d.right, d.bottom);
  BlitParams p = sprite;
  p.SetDst(o.tex);
  p.dst_rect = {static_cast<LONG>(std::lround(d.left * o.sx)), static_cast<LONG>(std::lround(d.top * o.sy)),
                static_cast<LONG>(std::lround(d.right * o.sx)), static_cast<LONG>(std::lround(d.bottom * o.sy))};
  p.erase = true;
  p.blend = p.premul = p.tint = false;
  p.opaque = false;
  p.linear = false;
  Blit(p);
}

void GpuTextCopy(const void* src, const RECT& sr, const void* dst, UINT dst_w, UINT dst_h, const RECT& dr, bool over) {
  if (!g_nonempty || !src || !dst) return;
  auto it = g_overlays.find(src);
  if (it == g_overlays.end() || it->second.empty) return;
  if (src == dst) {
    LogOnce("GPU text: text copied within one surface is not moved");
    return;
  }
  Overlay* d = EnsureOverlay(dst, dst_w, dst_h);
  it = g_overlays.find(src);  // EnsureOverlay may have added to the map
  if (!d || it == g_overlays.end()) return;
  const Overlay& s = it->second;
  BlitParams p;
  p.SetDst(d->tex);
  p.dst_rect = ScaleRect(*d, dr);
  p.SetSrc(s.tex);
  const bool same_size = (dr.right - dr.left) == (sr.right - sr.left) && (dr.bottom - dr.top) == (sr.bottom - sr.top);
  if (same_size && std::fabs(d->sx - s.sx) < 1e-4f && std::fabs(d->sy - s.sy) < 1e-4f) {
    // A plain copy: move by a whole number of overlay pixels, the same
    // distance both ways (copying away and back lands exactly), no filtering.
    const LONG ox = std::lround(sr.left * s.sx) - std::lround(dr.left * d->sx);
    const LONG oy = std::lround(sr.top * s.sy) - std::lround(dr.top * d->sy);
    p.src_rect = {p.dst_rect.left + ox, p.dst_rect.top + oy, p.dst_rect.right + ox, p.dst_rect.bottom + oy};
    // Keep inside the source overlay.
    const LONG cl = std::max<LONG>(0, -p.src_rect.left), ct = std::max<LONG>(0, -p.src_rect.top);
    const LONG cr = std::max<LONG>(0, p.src_rect.right - static_cast<LONG>(s.tex.width));
    const LONG cb = std::max<LONG>(0, p.src_rect.bottom - static_cast<LONG>(s.tex.height));
    p.src_rect.left += cl, p.dst_rect.left += cl;
    p.src_rect.top += ct, p.dst_rect.top += ct;
    p.src_rect.right -= cr, p.dst_rect.right -= cr;
    p.src_rect.bottom -= cb, p.dst_rect.bottom -= cb;
    if (p.dst_rect.left >= p.dst_rect.right || p.dst_rect.top >= p.dst_rect.bottom) return;
    p.linear = false;
  } else {
    p.src_rect = ScaleRect(s, sr);
    p.linear = true;
  }
  p.premul = over;
  p.opaque = false;
  Blit(p);
  MarkUsed(*d);
  ++g_ts.copies;
  TLog("copied text %ld,%ld-%ld,%ld -> %ld,%ld-%ld,%ld%s", sr.left, sr.top, sr.right, sr.bottom, dr.left, dr.top,
       dr.right, dr.bottom, over ? " (over)" : "");
}

bool GpuTextHasText(const void* native) {
  if (!g_nonempty || !native) return false;
  auto it = g_overlays.find(native);
  return it != g_overlays.end() && !it->second.empty;
}

void GpuTextComposite(const GpuTexture& src, ID3D11RenderTargetView* dst, IDirect3DSurface9* dst9, UINT dst_w,
                      UINT dst_h, const RECT& r) {
  Report();
  if (!g_nonempty) return;
  auto it = g_overlays.find(src.Native());
  if (it == g_overlays.end() || it->second.empty || !it->second.tex.Valid()) return;
  const GpuTexture& t = it->second.tex;
  BlitParams p;
  p.dst = dst;
  p.dst9 = dst9;
  p.dst_w = dst_w;
  p.dst_h = dst_h;
  p.dst_rect = r;
  p.SetSrc(t);
  p.src_rect = {0, 0, static_cast<LONG>(t.width), static_cast<LONG>(t.height)};
  p.linear = static_cast<LONG>(t.width) != r.right - r.left || static_cast<LONG>(t.height) != r.bottom - r.top;
  p.premul = true;
  p.opaque = false;
  Blit(p);
  ++g_ts.composites;
}

void GpuTextForget(const void* native) {
  if (!native || g_overlays.empty()) return;
  auto it = g_overlays.find(native);
  if (it == g_overlays.end()) return;
  MarkEmpty(it->second);
  GpuTexture t = it->second.tex;
  g_overlays.erase(it);
  t.Reset();
}

void GpuTextDeviceLost() {
  std::vector<GpuTexture> texs;
  for (auto& kv : g_overlays) texs.push_back(kv.second.tex);
  g_overlays.clear();
  g_nonempty = 0;
  for (GpuTexture& t : texs) t.Reset();
}

}  // namespace dd
