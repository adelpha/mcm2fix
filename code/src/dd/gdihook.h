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

class GdiTarget {
 public:
  virtual ~GdiTarget() = default;
  // r is clipped to the surface and not empty.
  virtual void Touch(const RECT& r) = 0;
};

void InstallGdiHooks();
// what: short description for the log ("gpu", "cpu", "d3d9 lazy").
void GdiTrack(HDC dc, GdiTarget* target, UINT w, UINT h, const char* what);
void GdiUntrack(HDC dc);
bool GdiTracked(HDC dc);
void GdiReportStats();

}  // namespace dd
