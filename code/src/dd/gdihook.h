// Hooks on the GDI calls Mcm2.exe draws onto DirectDraw surfaces with
// (TextOutA, DrawTextA, FillRect, FrameRect, MoveToEx/LineTo, SelectClipRgn).
//
// For every device context handed out by Surface::GetDC the hooks work out
// which pixels each call can touch. A GdiTarget registered for the DC is told
// before the call runs, so it can fetch just those pixels from the GPU first
// (the Direct3D 9 renderer does this instead of reading back the whole frame).
// With GdiLog=1 the calls are also logged (first ones in detail, then counts).
#pragma once
#include <windows.h>

namespace dd {

enum class GdiFn { kTextOut, kDrawText, kFillRect, kFrameRect, kLineTo };

// One drawing call, as the game made it.
struct GdiCall {
  GdiFn fn;
  HDC dc;
  int x, y;           // TextOut position, LineTo end point
  LPCSTR s;           // TextOut / DrawText string
  int len;            // its length (DrawText: -1 = zero-terminated)
  const RECT* rect;   // DrawText / FillRect / FrameRect
  UINT fmt;           // DrawText format
  HBRUSH brush;       // FillRect / FrameRect
};

class GdiTarget {
 public:
  virtual ~GdiTarget() = default;
  // r is clipped to the surface and not empty. Called before the call runs.
  virtual void Touch(const RECT&) {}
  // Draw the call some other way. true: GDI does not run it.
  virtual bool Draw(const GdiCall&) { return false; }
  // The game called SelectClipRgn on the DC.
  virtual void ClipChanged() {}
};

void InstallGdiHooks();
// what: short description for the log ("gpu", "cpu", "d3d9 lazy").
void GdiTrack(HDC dc, GdiTarget* target, UINT w, UINT h, const char* what);
void GdiUntrack(HDC dc);
bool GdiTracked(HDC dc);
void GdiReportStats();

}  // namespace dd
