// True-colour textures.
//
// The game converts every texture through its own format ids (Pixtrans.cpp
// converts between any pair of them): 8 = 8-bit paletted, 0x22b = RGB 555,
// 0x235 = RGB 565, 0x378 = RGB 888 (24-bit), 0x613 = ARGB 1555,
// 0x115c = ARGB 4444, 0x22b8 = ARGB 8888. Its texture loader (PCTexMap,
// 0x4cb110) is always asked for 16-bit formats, though: 4444 for 32-bit TGAs,
// 565/555 for 24-bit ones, and 8-bit terrain falls back to 555 when no
// paletted format is offered. And 8888 could never have worked anyway: the
// function that turns an id into a DDPIXELFORMAT (0x5170a0) describes 8888
// as 24 bits per pixel, so it never matches a real 32-bit format.
//
// With TrueColorTextures=1 we
//  1. replace 0x5170a0 with a copy that describes 8888 as 32 bits, and
//  2. hook the point in 0x4cb110 where the format has been chosen
//     (0x4cb189, eax = format id) and upgrade it: alpha formats (4444,
//     1555) -> 8888, opaque formats (565, 555, paletted) -> 888.
// The renderer (dd/device.cpp) offers 888 and 8888 texture formats too.
#include <windows.h>
#include <ddraw.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <utility>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kFormatDesc = 0x5170A0;
const uint8_t kFormatDescEntry[] = {0x8B, 0x54, 0x24, 0x08,         // mov edx,[esp+8]
                                    0x57,                           // push edi
                                    0xB9, 0x08, 0x00, 0x00, 0x00};  // mov ecx,8

constexpr uintptr_t kChosen = 0x4CB189;
constexpr uintptr_t kChosenResume = 0x4CB190;
const uint8_t kChosenCode[] = {0x8D, 0x94, 0x24, 0xA4, 0x00, 0x00, 0x00,  // lea edx,[esp+0A4h]
                               0x89, 0x45, 0x20};                         // mov [ebp+20h],eax

constexpr uintptr_t kFromList = 0x4CB030;
const uint8_t kFromListEntry[] = {0x53,                     // push ebx
                                  0x8B, 0x5C, 0x24, 0x10};  // mov ebx,[esp+10h]

constexpr int kPal8 = 0x8, kRgb555 = 0x22b, kRgb565 = 0x235, kRgb888 = 0x378, kArgb1555 = 0x613,
              kArgb4444 = 0x115c, kArgb8888 = 0x22b8;

const char* Name(int id) {
  switch (id) {
    case kPal8: return "PAL8";
    case kRgb555: return "555";
    case kRgb565: return "565";
    case kRgb888: return "888";
    case kArgb1555: return "1555";
    case kArgb4444: return "4444";
    case kArgb8888: return "8888";
    default: return "?";
  }
}

// cdecl, like the original: fill pf (32 bytes) for format id.
void __cdecl FormatDesc(int id, DDPIXELFORMAT* pf) {
  std::memset(pf, 0, sizeof(*pf));
  pf->dwSize = sizeof(*pf);
  pf->dwFlags = DDPF_RGB;
  auto rgb = [&](DWORD bits, DWORD r, DWORD g, DWORD b, DWORD a) {
    pf->dwRGBBitCount = bits;
    pf->dwRBitMask = r;
    pf->dwGBitMask = g;
    pf->dwBBitMask = b;
    if (a) {
      pf->dwFlags |= DDPF_ALPHAPIXELS;
      pf->dwRGBAlphaBitMask = a;
    }
  };
  switch (id) {
    case kPal8:
      pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
      pf->dwRGBBitCount = 8;
      break;
    case kRgb555: rgb(16, 0x7c00, 0x03e0, 0x001f, 0); break;
    case kRgb565: rgb(16, 0xf800, 0x07e0, 0x001f, 0); break;
    case kRgb888: rgb(24, 0xff0000, 0xff00, 0xff, 0); break;
    case kArgb1555: rgb(16, 0x7c00, 0x03e0, 0x001f, 0x8000); break;
    case kArgb4444: rgb(16, 0x0f00, 0x00f0, 0x000f, 0xf000); break;
    case kArgb8888: rgb(32, 0xff0000, 0xff00, 0xff, 0xff000000); break;  // the game said 24
    default: break;
  }
}

// The format worth upgrading to, given what the picture holds in memory
// (src) and what the loader asked for (id). Only an upgrade that keeps
// more of the source is worth it: most of the game's pictures are already
// 555 in memory (24-bit TGAs are cut down when loaded), so a 24-bit
// surface would hold exactly the same colours.
int Upgrade(int src, int id) {
  switch (id) {
    case kArgb4444:
    case kArgb1555: return src == kArgb8888 ? kArgb8888 : id;
    case kRgb565:
    case kRgb555:
    case kPal8: return (src == kRgb888 || src == kArgb8888) ? kRgb888 : id;
    default: return id;
  }
}

// Logging: every texture in detail for the first kDetailed ones, then a
// summary of (asked -> used) counts every kSummaryEvery textures.
constexpr unsigned kDetailed = 400;
constexpr unsigned kSummaryEvery = 250;
unsigned g_count = 0;
std::map<std::pair<int, int>, unsigned> g_pairs;

void Summary() {
  for (const auto& p : g_pairs)
    patch::Log("textures: %s -> %s x%u", Name(p.first.first), Name(p.first.second), p.second);
}

// stdcall: called from the thunk with eax (chosen format), ebp (the texture
// object) and esp at the hook point. Returns the format to use.
int g_pending = 0;  // format OnCreateFromList should try first, 0 = none
int __stdcall OnFormatChosen(int id, uintptr_t tex, uintptr_t esp) {
  // 0x4cb110 has pushed 4 registers after "sub esp,0C4h": the return
  // address is at +0D4h, argument k at +0D4h + 4k.
  const auto* a = reinterpret_cast<const uint32_t*>(esp + 0xd4);
  // Argument 9 bit 2: the picture is a plain off-screen surface in the
  // display's own format (cursor, 2D blits). The game creates those without
  // a pixel format, so they are 16-bit whatever we choose; upgrading them
  // made the game write 24-bit pixels into a 16-bit buffer (truecolor2
  // crashed on the mouse cursor). Leave them alone.
  const bool display_format = (a[9] & 4) != 0;
  // Argument 1 = 0: the caller (0x4ca7d0) copies the pixels in itself and
  // works out the row pitch from the 16-bit format it asked for, so a
  // 24-bit surface got overrun (truecolor4 crashed at the Microsoft logo).
  const bool caller_uploads = a[1] == 0;
  const int src = static_cast<int>(a[6]);
  const char* why = "";
  int out = id;
  if (!GetConfig().true_color_textures) why = " (off)";
  else if (display_format) why = " (display format, kept)";
  else if (caller_uploads) why = " (caller copies pixels itself, kept)";
  else {
    out = Upgrade(src, id);
    if (out == id) why = " (no gain from source, kept)";
  }
  // The upgrade itself happens in OnCreateFromList (0x4cb030), which
  // 0x4cb110 calls next for this texture.
  g_pending = out != id ? out : 0;
  ++g_count;
  ++g_pairs[{id, out}];
  if (GetConfig().texture_log && g_count <= kDetailed) {
    patch::Log("texture #%u: format %s(0x%x) -> %s%s caller=%08x tex=%08x args %x %x %x %x %x %x %x %x %x %x",
               g_count, Name(id), id, Name(out), why, a[0], static_cast<unsigned>(tex), a[1], a[2], a[3], a[4],
               a[5], a[6], a[7], a[8], a[9], a[10]);
  }
  if (GetConfig().texture_log && g_count % kSummaryEvery == 0) {
    patch::Log("textures: %u created so far", g_count);
    Summary();
  }
  return id;
}

// 0x4cb030 (thiscall, ret 0Ch): tex->CreateFromList(desc, flags, list):
// tries each format id of the 0-terminated list in turn (sets tex+20h,
// fills desc's pixel format when desc asks for one, checks the device
// supports it, creates the surface, counts its memory). Both of the
// loader's paths (plain and mipmapped) go through it, so this is where we
// put the true-colour format in front of the game's own list. Surfaces
// created without a pixel format are in the display format and keep the
// game's list.
uint32_t g_list[8];
unsigned g_list_log = 0;
const uint32_t* __stdcall OnCreateFromList(uintptr_t tex, const DDSURFACEDESC2* desc,
                                           const uint32_t* list) {
  const int up = g_pending;
  g_pending = 0;
  if (!up || !desc || !list || !list[0]) return list;
  if (!(desc->dwFlags & DDSD_PIXELFORMAT)) {
    if (GetConfig().texture_log && ++g_list_log <= kDetailed)
      patch::Log("texture %08x: no pixel format in desc, %s not tried", static_cast<unsigned>(tex), Name(up));
    return list;
  }
  if (up == static_cast<int>(list[0])) return list;
  size_t n = 0;
  g_list[n++] = static_cast<uint32_t>(up);
  for (size_t i = 0; list[i] && n < 7; ++i) g_list[n++] = list[i];
  g_list[n] = 0;
  if (GetConfig().texture_log && ++g_list_log <= kDetailed)
    patch::Log("texture %08x: %lux%lu caps %08lx: trying %s before the game's %s%s%s",
               static_cast<unsigned>(tex), desc->dwWidth, desc->dwHeight, desc->ddsCaps.dwCaps,
               Name(up), Name(static_cast<int>(list[0])), list[1] ? "/" : "",
               list[1] ? Name(static_cast<int>(list[1])) : "");
  return g_list;
}

}  // namespace

void ApplyTextureFixes() {
  const Config& c = GetConfig();
  patch::Log("textures: TrueColorTextures=%d TextureLog=%d", c.true_color_textures ? 1 : 0,
             c.texture_log ? 1 : 0);

  if (c.true_color_textures) {
    if (patch::Matches(kFormatDesc, kFormatDescEntry, sizeof(kFormatDescEntry))) {
      patch::Jump(kFormatDesc, reinterpret_cast<uintptr_t>(&FormatDesc), 5);
      patch::Log("textures: format descriptions replaced (8888 = 32 bits)");
    } else {
      patch::Log("textures: format description code differs, not patched");
    }
  }

  if (!c.true_color_textures && !c.texture_log) return;
  if (!patch::Matches(kChosen, kChosenCode, sizeof(kChosenCode))) {
    patch::Log("textures: texture loader code differs, format hook not applied");
    return;
  }
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  size_t n = 0;
  auto put = [&](std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) t[n++] = x;
  };
  auto put32 = [&](uint32_t v) {
    std::memcpy(t + n, &v, 4);
    n += 4;
  };
  put({0x60});                    // pushad
  put({0xFF, 0x74, 0x24, 0x0C});  // push [esp+0Ch]   (esp before pushad)
  put({0x55});                    // push ebp         (texture object)
  put({0x50});                    // push eax         (format id)
  put({0xB8});
  put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnFormatChosen)));  // mov eax, fn
  put({0xFF, 0xD0});              // call eax
  put({0x89, 0x44, 0x24, 0x1C});  // mov [esp+1Ch],eax (pushad's eax slot)
  put({0x61});                    // popad
  put({0x8D, 0x94, 0x24, 0xA4, 0x00, 0x00, 0x00});  // lea edx,[esp+0A4h]
  put({0x68});
  put32(static_cast<uint32_t>(kChosenResume));  // push 0x4cb190
  put({0xC3});                                  // ret
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(kChosen, reinterpret_cast<uintptr_t>(t), 7);
  patch::Log("textures: format log hook applied at %08x", static_cast<unsigned>(kChosen));

  if (!c.true_color_textures) return;
  if (!patch::Matches(kFromList, kFromListEntry, sizeof(kFromListEntry))) {
    patch::Log("textures: create-from-list code differs, true colour not applied");
    return;
  }
  auto* u = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!u) return;
  n = 0;
  t = u;
  put({0x60});                    // pushad
  put({0xFF, 0x74, 0x24, 0x2C});  // push [esp+2Ch]  (list: arg 3)
  put({0xFF, 0x74, 0x24, 0x28});  // push [esp+28h]  (desc: arg 1)
  put({0x51});                    // push ecx        (texture object)
  put({0xB8});
  put32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnCreateFromList)));  // mov eax, fn
  put({0xFF, 0xD0});              // call eax
  put({0x89, 0x44, 0x24, 0x2C});  // mov [esp+2Ch],eax (replace arg 3)
  put({0x61});                    // popad
  for (uint8_t b : kFromListEntry) put({b});  // push ebx; mov ebx,[esp+10h]
  put({0x68});
  put32(static_cast<uint32_t>(kFromList + sizeof(kFromListEntry)));  // push 0x4cb035
  put({0xC3});                                                       // ret
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Jump(kFromList, reinterpret_cast<uintptr_t>(t), sizeof(kFromListEntry));
  patch::Log("textures: true-colour list hook applied at %08x", static_cast<unsigned>(kFromList));
}

void LogTextureSummary() {
  if (!g_count) return;
  patch::Log("textures: %u created in total", g_count);
  Summary();
}
