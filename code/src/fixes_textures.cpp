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
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include "config.h"
#include "dd/texreplace.h"
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
// ---- Texture names ----
//
// The game loads textures by name: TextureMap loader 0x50fd00(?, name, ...)
// looks the name up in the .RES archives (the texture cache loader 0x4ca7d0
// then reads the stored mip levels) and falls back to a TGA file; other
// code loads TGA files directly with 0x517ca0(name, ...) or
// 0x5173e0(name, ...), which return an image whose pixels (+14h) are later
// handed to the texture creator 0x4cb110 as argument 1. We remember those
// names and pass the one that belongs to each texture to the renderer, which
// dumps and replaces textures by name (dd/texreplace.cpp).
constexpr uintptr_t kLoadTexMap = 0x50FD00;
const uint8_t kLoadTexMapEntry[] = {0x6A, 0xFF, 0x68, 0xBE, 0x3F, 0x55, 0x00};  // push -1; push 553FBEh
constexpr uintptr_t kLoadTga = 0x517CA0;
const uint8_t kLoadTgaEntry[] = {0x6A, 0xFF, 0x68, 0x27, 0x44, 0x55, 0x00};  // push -1; push 554427h
constexpr uintptr_t kLoadTga2 = 0x5173E0;
const uint8_t kLoadTga2Entry[] = {0x6A, 0xFF, 0x68, 0xF7, 0x43, 0x55, 0x00};  // push -1; push 5543F7h

using LoadTexMapFn = uintptr_t(__cdecl*)(uintptr_t, const char*, uintptr_t, uintptr_t, uintptr_t,
                                         uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                         uintptr_t, uintptr_t);
using LoadTgaFn = uintptr_t(__cdecl*)(const char*, uintptr_t, uintptr_t);
LoadTexMapFn g_load_texmap = nullptr;
LoadTgaFn g_load_tga = nullptr;
LoadTgaFn g_load_tga2 = nullptr;

std::string g_loading;  // name being loaded by 0x50fd00, empty outside it
std::unordered_map<uintptr_t, std::string> g_data_names;  // image pixels -> file name
std::unordered_map<uintptr_t, std::string> g_obj_names;   // texture object -> name
std::string g_next_name;  // name for the surface 0x4cb030 is about to create
unsigned g_named = 0, g_unnamed = 0, g_by_loader = 0, g_by_file = 0, g_by_object = 0;

bool Readable(const void* p, size_t n) {
  MEMORY_BASIC_INFORMATION mi{};
  if (!p || !VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return false;
  if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const auto end = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
  return reinterpret_cast<uintptr_t>(p) + n <= end;
}

// A printable name of at most 127 characters, or "".
std::string SafeName(const char* p) {
  if (!Readable(p, 1)) return "";
  std::string s;
  for (size_t i = 0; i < 128; ++i) {
    if (!Readable(p + i, 1)) return "";
    const char c = p[i];
    if (!c) break;
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) return "";
    s += c;
  }
  return s.size() < 128 ? s : "";
}

#ifdef _MSC_VER
#include <intrin.h>
#define ReturnAddress() _ReturnAddress()
#else
#define ReturnAddress() __builtin_return_address(0)
#endif

uintptr_t __cdecl LoadTexMapHook(uintptr_t a1, const char* name, uintptr_t a3, uintptr_t a4,
                                 uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,
                                 uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12) {
  std::string saved;
  saved.swap(g_loading);
  g_loading = SafeName(name);
  const uintptr_t r = g_load_texmap(a1, name, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
  if (r && !g_loading.empty()) g_obj_names[r] = g_loading;
  static unsigned logged = 0;
  if (GetConfig().texture_log && ++logged <= 300)
    patch::Log("texture map \"%s\" -> %08x (called from %08x)", g_loading.c_str(), static_cast<unsigned>(r),
               static_cast<unsigned>(reinterpret_cast<uintptr_t>(ReturnAddress())));
  g_loading.swap(saved);
  return r;
}

void RememberImage(const char* name, uintptr_t img, const char* which) {
  if (!img || !Readable(reinterpret_cast<void*>(img + 0x14), 4)) return;
  const uintptr_t px = *reinterpret_cast<const uintptr_t*>(img + 0x14);
  std::string n = SafeName(name);
  if (!px || n.empty()) return;
  static unsigned logged = 0;
  if (GetConfig().texture_log && ++logged <= 300)
    patch::Log("texture file \"%s\" (%s) pixels %08x", n.c_str(), which, static_cast<unsigned>(px));
  g_data_names[px] = std::move(n);
}

uintptr_t __cdecl LoadTgaHook(const char* name, uintptr_t a2, uintptr_t a3) {
  const uintptr_t r = g_load_tga(name, a2, a3);
  RememberImage(name, r, "517ca0");
  return r;
}

uintptr_t __cdecl LoadTga2Hook(const char* name, uintptr_t a2, uintptr_t a3) {
  const uintptr_t r = g_load_tga2(name, a2, a3);
  RememberImage(name, r, "5173e0");
  return r;
}

// TextureMap base constructor 0x50fc50 (thiscall): every texture object is
// built through it, so a new object at a reused address loses the old name.
constexpr uintptr_t kTexMapCtor = 0x50FC50;
const uint8_t kTexMapCtorEntry[] = {0x6A, 0xFF, 0x68, 0x78, 0x3F, 0x55, 0x00};  // push -1; push 553F78h
unsigned g_forgotten = 0;
void __stdcall ForgetObject(uintptr_t obj) {
  if (g_obj_names.erase(obj)) ++g_forgotten;
}

// ---- Textures read straight from a file stream ----
// The terrain tiles (0x43e400) and the texture cache (around 0x50b7c9) hand
// the texture reader 0x4ca7d0 an open stream (argument 1) positioned at the
// texture, without a name. Streams are opened by name with 0x461ec0
// (thiscall, argument 1 = name), so we remember that name per stream and
// call such a texture "<file>_<offset>", which stays the same from run to
// run.
constexpr uintptr_t kStreamOpen = 0x461EC0;
const uint8_t kStreamOpenEntry[] = {0x81, 0xEC, 0x90, 0x03, 0x00, 0x00};  // sub esp,390h
constexpr uintptr_t kTexRead = 0x4CA7D0;
const uint8_t kTexReadEntry[] = {0x81, 0xEC, 0xDC, 0x00, 0x00, 0x00};  // sub esp,0DCh
constexpr uintptr_t kTexReadCall = 0x4CA99C;  // its call to 0x4cb110 returns here
using TellFn = int(__thiscall*)(uintptr_t);
const TellFn kStreamTell = reinterpret_cast<TellFn>(0x462570);

std::unordered_map<uintptr_t, std::string> g_stream_names;
std::string g_stream_tex;  // name for the texture 0x4ca7d0 is reading, until it creates it
unsigned g_by_stream = 0;

void __stdcall OnStreamOpen(uintptr_t stream, const char* name) {
  std::string n = SafeName(name);
  if (n.empty()) return;
  static unsigned logged = 0;
  if (GetConfig().texture_log && ++logged <= 100)
    patch::Log("stream %08x opened \"%s\"", static_cast<unsigned>(stream), n.c_str());
  g_stream_names[stream] = std::move(n);
}

void __stdcall OnTexRead(uintptr_t self, uintptr_t stream) {
  (void)self;
  g_stream_tex.clear();
  if (!stream) return;
  // A sub-stream points to its parent at +1Ch; use the first named one.
  const std::string* file = nullptr;
  for (uintptr_t s = stream; s && !file; ) {
    auto it = g_stream_names.find(s);
    if (it != g_stream_names.end()) file = &it->second;
    if (!Readable(reinterpret_cast<void*>(s + 0x1c), 4)) break;
    const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(s + 0x1c);
    if (parent == s) break;
    s = parent;
  }
  if (!file) return;
  // 0x462570 reads the position of the root stream's FILE (+14h): only
  // call it when that is open.
  uintptr_t root = stream;
  for (int i = 0; i < 16; ++i) {
    if (!Readable(reinterpret_cast<void*>(root + 0x1c), 4)) return;
    const uintptr_t up = *reinterpret_cast<const uintptr_t*>(root + 0x1c);
    if (!up) break;
    root = up;
  }
  if (!Readable(reinterpret_cast<void*>(root + 0x14), 4) || !*reinterpret_cast<const uintptr_t*>(root + 0x14)) return;
  const int pos = kStreamTell(stream);
  if (pos < 0) return;
  std::string base = *file;
  const size_t dot = base.find_last_of('.');
  const size_t sep = base.find_last_of("\\/");
  if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) base.resize(dot);
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "_%x", static_cast<unsigned>(pos));
  g_stream_tex = base + suffix;
}

// pushad; push [esp+24h] (argument 1); push ecx; call fn (stdcall, 2 args);
// popad; the moved entry bytes; jump back.
bool HookThiscallArg1(const char* what, uintptr_t addr, const uint8_t* entry, size_t len, void* fn) {
  if (!patch::Matches(addr, entry, len)) {
    patch::Log("textures: %s code differs, not hooked", what);
    return false;
  }
  auto* k = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!k) return false;
  size_t m = 0;
  auto emit = [&](std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) k[m++] = x;
  };
  auto emit32 = [&](uint32_t v) {
    std::memcpy(k + m, &v, 4);
    m += 4;
  };
  emit({0x60, 0xFF, 0x74, 0x24, 0x24, 0x51, 0xB8});  // pushad; push [esp+24h]; push ecx; mov eax, fn
  emit32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn)));
  emit({0xFF, 0xD0, 0x61});  // call eax; popad
  for (size_t i = 0; i < len; ++i) emit({entry[i]});
  emit({0x68});
  emit32(static_cast<uint32_t>(addr + len));
  emit({0xC3});
  FlushInstructionCache(GetCurrentProcess(), k, m);
  patch::Jump(addr, reinterpret_cast<uintptr_t>(k), len);
  patch::Log("textures: %s hooked at %08x", what, static_cast<unsigned>(addr));
  return true;
}

// Which name the texture being created (0x4cb110 on `tex`, pixels a1) has.
const char* NameTexture(uintptr_t tex, uintptr_t pixels, uintptr_t caller, std::string& out) {
  out.clear();
  if (pixels) {
    auto it = g_data_names.find(pixels);
    if (it != g_data_names.end()) {
      out = it->second;
      ++g_by_file;
      g_obj_names[tex] = out;
      return "file";
    }
  }
  if (!g_loading.empty()) {
    out = g_loading;
    ++g_by_loader;
    g_obj_names[tex] = out;
    return "loader";
  }
  if (!pixels && caller == kTexReadCall && !g_stream_tex.empty()) {
    out.swap(g_stream_tex);
    g_stream_tex.clear();
    ++g_by_stream;
    g_obj_names[tex] = out;
    return "stream";
  }
  auto it = g_obj_names.find(tex);
  if (it != g_obj_names.end()) {
    if (pixels) {
      // New pixels from somewhere we don't know: the object was reused.
      g_obj_names.erase(it);
      return "none";
    }
    out = it->second;
    ++g_by_object;
    return "object";
  }
  return "none";
}

void* MakeTrampoline(uintptr_t addr, const uint8_t* entry, size_t len) {
  auto* t = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return nullptr;
  std::memcpy(t, entry, len);
  t[len] = 0xE9;
  const int32_t rel = static_cast<int32_t>((addr + len) - (reinterpret_cast<uintptr_t>(t) + len + 5));
  std::memcpy(t + len + 1, &rel, 4);
  FlushInstructionCache(GetCurrentProcess(), t, len + 5);
  return t;
}

bool HookEntry(const char* what, uintptr_t addr, const uint8_t* entry, size_t len, void* hook,
               void** tramp) {
  if (!patch::Matches(addr, entry, len)) {
    patch::Log("textures: %s code differs, not hooked", what);
    return false;
  }
  *tramp = MakeTrampoline(addr, entry, len);
  if (!*tramp) return false;
  patch::Jump(addr, reinterpret_cast<uintptr_t>(hook), len);
  patch::Log("textures: %s hooked at %08x", what, static_cast<unsigned>(addr));
  return true;
}

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
  std::string tex_name;
  const char* name_src = NameTexture(tex, a[1], a[0], tex_name);
  if (tex_name.empty()) ++g_unnamed;
  else ++g_named;
  if (tex_name.empty() && !display_format && GetConfig().texture_log) {
    // Who created an unnamed texture: the return addresses on the stack
    // (values in the game's code that follow a call instruction).
    static unsigned traced = 0;
    if (++traced <= 150) {
      char buf[160];
      int len = 0;
      const auto* p = reinterpret_cast<const uint32_t*>(esp);
      int found = 0;
      for (int i = 0; i < 1024 && found < 8; ++i) {
        if (!Readable(p + i, 4)) break;
        const uint32_t v = p[i];
        if (v < 0x401005 || v >= 0x555000) continue;
        const auto* c = reinterpret_cast<const uint8_t*>(v);
        if (c[-5] != 0xE8 && !(c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) &&
            !(c[-3] == 0xFF && (c[-2] & 0x38) == 0x10) && !(c[-2] == 0xFF && (c[-1] & 0x38) == 0x10))
          continue;
        len += std::snprintf(buf + len, sizeof(buf) - len, " %08x", v);
        ++found;
      }
      buf[len] = 0;
      patch::Log("texture #%u unnamed (%ux%u), call chain:%s", g_count + 1, a[2], a[3], buf);
    }
  }
  // Display-format surfaces are not textures to the renderer; the name is
  // only used for texture surfaces anyway.
  g_next_name = dd::TextureKey(tex_name.c_str());
  dd::SetPendingTextureName(nullptr);
  ++g_count;
  ++g_pairs[{id, out}];
  if (GetConfig().texture_log && g_count <= kDetailed) {
    patch::Log("texture #%u: \"%s\" (%s) format %s(0x%x) -> %s%s caller=%08x tex=%08x args %x %x %x %x %x %x %x %x %x %x",
               g_count, tex_name.c_str(), name_src, Name(id), id, Name(out), why, a[0], static_cast<unsigned>(tex), a[1], a[2], a[3], a[4],
               a[5], a[6], a[7], a[8], a[9], a[10]);
  }
  if (GetConfig().texture_log && g_count % kSummaryEvery == 0) {
    patch::Log("textures: %u created so far (named: %u by file, %u by loader, %u by stream, %u by object; %u unnamed)",
               g_count, g_by_file, g_by_loader, g_by_stream, g_by_object, g_unnamed);
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
  // The surface the game creates next belongs to this texture.
  if (!g_next_name.empty()) dd::SetPendingTextureName(g_next_name.c_str());
  g_next_name.clear();
  const int up = GetConfig().true_color_textures ? g_pending : 0;
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

  const bool names = c.texture_replace || c.texture_dump;
  if (!c.true_color_textures && !c.texture_log && !names) return;
  if (names) {
    HookEntry("texture map loader", kLoadTexMap, kLoadTexMapEntry, sizeof(kLoadTexMapEntry),
              reinterpret_cast<void*>(&LoadTexMapHook), reinterpret_cast<void**>(&g_load_texmap));
    HookEntry("TGA loader", kLoadTga, kLoadTgaEntry, sizeof(kLoadTgaEntry),
              reinterpret_cast<void*>(&LoadTgaHook), reinterpret_cast<void**>(&g_load_tga));
    if (patch::Matches(kTexMapCtor, kTexMapCtorEntry, sizeof(kTexMapCtorEntry))) {
      auto* k = static_cast<uint8_t*>(
          VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
      if (k) {
        size_t m = 0;
        auto emit = [&](std::initializer_list<uint8_t> b) {
          for (uint8_t x : b) k[m++] = x;
        };
        auto emit32 = [&](uint32_t v) {
          std::memcpy(k + m, &v, 4);
          m += 4;
        };
        emit({0x60, 0x51, 0xB8});  // pushad; push ecx; mov eax, fn
        emit32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ForgetObject)));
        emit({0xFF, 0xD0, 0x61});  // call eax; popad
        for (uint8_t b : kTexMapCtorEntry) emit({b});
        emit({0x68});
        emit32(static_cast<uint32_t>(kTexMapCtor + sizeof(kTexMapCtorEntry)));
        emit({0xC3});
        FlushInstructionCache(GetCurrentProcess(), k, m);
        patch::Jump(kTexMapCtor, reinterpret_cast<uintptr_t>(k), sizeof(kTexMapCtorEntry));
        patch::Log("textures: texture object constructor hooked at %08x", static_cast<unsigned>(kTexMapCtor));
      }
    } else {
      patch::Log("textures: texture object constructor differs, not hooked");
    }
    HookThiscallArg1("stream open", kStreamOpen, kStreamOpenEntry, sizeof(kStreamOpenEntry),
                     reinterpret_cast<void*>(&OnStreamOpen));
    HookThiscallArg1("texture reader", kTexRead, kTexReadEntry, sizeof(kTexReadEntry),
                     reinterpret_cast<void*>(&OnTexRead));
    HookEntry("TGA file loader", kLoadTga2, kLoadTga2Entry, sizeof(kLoadTga2Entry),
              reinterpret_cast<void*>(&LoadTga2Hook), reinterpret_cast<void**>(&g_load_tga2));
  }
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

  if (!c.true_color_textures && !names) return;
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
  patch::Log("textures: create-from-list hook applied at %08x (true colour %s, names %s)",
             static_cast<unsigned>(kFromList), c.true_color_textures ? "on" : "off", names ? "on" : "off");
}

void LogTextureSummary() {
  if (!g_count) return;
  patch::Log("textures: %u created in total (named: %u by file, %u by loader, %u by stream, %u by object; %u unnamed; %u names dropped on reuse)",
             g_count, g_by_file, g_by_loader, g_by_stream, g_by_object, g_unnamed, g_forgotten);
  Summary();
}
