#include "hudtext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../config.h"
#include "../patch.h"
#include "backend.h"
#include "gputext.h"
#include "surface.h"

namespace dd {
namespace {

// The game's HUD panel object (thiscall, ecx): its texture wrappers. Each
// wrapper holds the IDirectDrawSurface7 at +0x70.
constexpr uintptr_t kConvert = 0x4bafd0;  // (int first_row, int rows, colour); ret 0xc
constexpr uintptr_t kRestore = 0x4bae60;  // (const RECT*); ret 4
constexpr uintptr_t kPanelTexture = 0x2c;
constexpr uintptr_t kPanelScratch = 0x34;
constexpr uintptr_t kWrapperSurface = 0x70;
constexpr uintptr_t kConvertCalls[] = {0x51f43c, 0x51faa7, 0x520067, 0x5204b0, 0x5236d9, 0x523824, 0x52407b};
constexpr uintptr_t kRestoreCalls[] = {0x51f2f7, 0x51f952, 0x51ff2c, 0x52039e, 0x523473,
                                       0x5238f0, 0x523e9a, 0x523f99, 0x523fc1};

using ConvertFn = void(__fastcall*)(uintptr_t self, void* edx, int first_row, int rows, DWORD colour);
using RestoreFn = void(__fastcall*)(uintptr_t self, void* edx, const RECT* r);
const ConvertFn g_convert = reinterpret_cast<ConvertFn>(kConvert);
const RestoreFn g_restore = reinterpret_cast<RestoreFn>(kRestore);

struct Stats {
  unsigned converts = 0, baked = 0, no_text = 0, restores = 0, rebuilds = 0, samples = 0, skipped = 0;
} g_st;
DWORD g_last_report = 0;

void Report() {
  const DWORD now = GetTickCount();
  if (!g_last_report) g_last_report = now;
  if (now - g_last_report < 5000) return;
  g_last_report = now;
  if (!g_st.converts && !g_st.restores && !g_st.samples) return;
  Log("hud text 5s: %u text conversions (%u drawn sharp, %u without text), %u wipes, %u full rebuilds, "
      "%u draws with a sharp panel, %u skipped",
      g_st.converts, g_st.baked, g_st.no_text, g_st.restores, g_st.rebuilds, g_st.samples, g_st.skipped);
  g_st = Stats{};
}

Surface* SurfaceAt(uintptr_t self, uintptr_t field) {
  if (!self) return nullptr;
  const uintptr_t wrapper = *reinterpret_cast<const uintptr_t*>(self + field);
  if (!wrapper) return nullptr;
  auto* dds = *reinterpret_cast<IDirectDrawSurface7* const*>(wrapper + kWrapperSurface);
  return dds ? Surface::From(dds) : nullptr;
}

// Make the sharper copy afresh from the panel as it is now.
bool Rebuild(Surface* t) {
  GpuTexture* g = t->Gpu();
  if (!g->Valid() || !g->CanSample()) return false;
  const float s = GpuTextTextureScale();
  const UINT w = std::max<UINT>(1, static_cast<UINT>(std::lround(t->Width() * s)));
  const UINT h = std::max<UINT>(1, static_cast<UINT>(std::lround(t->Height() * s)));
  GpuTexture* hi = t->Hires();
  if (!hi->Valid() || hi->width != w || hi->height != h) {
    hi->Reset();
    if (!CreateGpuTexture(*hi, w, h, 1, true, false, true)) {
      LogOnce("hud text: a %ux%u copy could not be made", w, h);
      return false;
    }
    Log("hud text: %ux%u panel texture gets a %ux%u sharp copy (x%.2f)", t->Width(), t->Height(), w, h, s);
  }
  BlitParams p;
  p.SetDst(*hi);
  p.dst_rect = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
  p.SetSrc(*g);
  p.src_rect = {0, 0, static_cast<LONG>(t->Width()), static_cast<LONG>(t->Height())};
  p.linear = true;
  p.opaque = false;
  Blit(p);
  t->SetHiresSerial(t->ContentSerial());
  ++g_st.rebuilds;
  return true;
}

bool Active() { return GpuTextActive() && GetConfig().hud_text; }

void __fastcall ConvertHook(uintptr_t self, void* edx, int first_row, int rows, DWORD colour) {
  Surface* t = Active() ? SurfaceAt(self, kPanelTexture) : nullptr;
  Surface* s = t ? SurfaceAt(self, kPanelScratch) : nullptr;
  if (!t || !s || rows <= 0) {
    if (Active()) ++g_st.skipped;
    g_convert(self, edx, first_row, rows, colour);
    return;
  }
  Report();
  ++g_st.converts;
  // Bring the sharp copy up to date with the panel before the text goes in.
  t->Gpu();
  if (!t->HiresCurrent()) Rebuild(t);
  // A pixel the game is about to turn into text: its colour afterwards is
  // the text colour (in the panel's own format, alpha included).
  const LONG y0 = std::max(first_row, 0);
  const LONG y1 = std::min<LONG>(first_row + rows, static_cast<LONG>(s->Height()));
  LONG px = -1, py = -1;
  for (LONG y = y0; y < y1 && px < 0; ++y)
    for (LONG x = 0; x < static_cast<LONG>(s->Width()); ++x) {
      uint8_t c[4];
      if (s->CpuPixelBGRA(x, y, c) && c[0] >= 0xf0 && c[1] >= 0xf0 && c[2] >= 0xf0) {
        px = x;
        py = y;
        break;
      }
    }
  const bool was_current = t->HiresCurrent();
  g_convert(self, edx, first_row, rows, colour);
  t->Gpu();
  const RECT r{0, y0, static_cast<LONG>(s->Width()), y1};
  if (!was_current) {
    GpuTextOcclude(s, &r, "converted");
    return;
  }
  float rgba[4] = {1, 1, 1, 1};
  uint8_t c[4];
  if (px >= 0 && t->CpuPixelBGRA(px, py, c)) {
    rgba[0] = c[2] / 255.0f;
    rgba[1] = c[1] / 255.0f;
    rgba[2] = c[0] / 255.0f;
    rgba[3] = t->Layout().mask[3] ? c[3] / 255.0f : 1.0f;
  }
  if (px >= 0 && GpuTextBake(s, r, *t->Hires(), rgba)) {
    ++g_st.baked;
    static unsigned logged = 0;
    if (++logged <= 20)
      Log("hud text: rows %ld-%ld of a %ux%u panel drawn sharp in %02x%02x%02x alpha %.2f", y0, y1, t->Width(),
          t->Height(), c[2], c[1], c[0], rgba[3]);
  } else {
    ++g_st.no_text;
  }
  // The game cleared the converted pixels of the scratch texture.
  GpuTextOcclude(s, &r, "converted");
  t->SetHiresSerial(t->ContentSerial());
}

void __fastcall RestoreHook(uintptr_t self, void* edx, const RECT* rect) {
  Surface* t = Active() ? SurfaceAt(self, kPanelTexture) : nullptr;
  if (!t || !rect) {
    g_restore(self, edx, rect);
    return;
  }
  Report();
  ++g_st.restores;
  t->Gpu();
  const bool was_current = t->HiresCurrent();
  g_restore(self, edx, rect);
  GpuTexture* g = t->Gpu();
  if (!was_current || !g->CanSample()) return;
  // The panel's background came back in rect: copy it over, scaled up.
  GpuTexture* hi = t->Hires();
  const float kx = static_cast<float>(hi->width) / t->Width(), ky = static_cast<float>(hi->height) / t->Height();
  BlitParams p;
  p.SetDst(*hi);
  p.dst_rect = {static_cast<LONG>(std::lround(rect->left * kx)), static_cast<LONG>(std::lround(rect->top * ky)),
                static_cast<LONG>(std::lround(rect->right * kx)), static_cast<LONG>(std::lround(rect->bottom * ky))};
  p.SetSrc(*g);
  p.src_rect = *rect;
  p.linear = true;
  p.opaque = false;
  if (p.dst_rect.left < p.dst_rect.right && p.dst_rect.top < p.dst_rect.bottom) Blit(p);
  t->SetHiresSerial(t->ContentSerial());
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

}  // namespace

void NoteHiresSample() { ++g_st.samples; }

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
  Log("hud text: %u of %u HUD panel calls hooked", ok, total);
}

}  // namespace dd
