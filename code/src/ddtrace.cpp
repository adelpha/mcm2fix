// Diagnostics for step 3: record which DirectDraw 7 / Direct3D 7 methods the
// game calls, and with which important parameters, so the D3D11 backend
// covers exactly that set.
//
// The game creates DirectDraw through one import (blade.dll's
// DirectDrawCreateEx). We hook it, then patch the method tables (vtables) of
// each interface the game obtains, in place. Every slot gets a tiny counting
// thunk; a few slots get a C++ handler that also logs parameters and
// patches the objects they return. Counts are written to the log when the
// game exits (and every 4096 flips).
#include <windows.h>
#include <initguid.h>  // define the IIDs used below in this file
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include "patch.h"

namespace {

constexpr uintptr_t kIatDirectDrawCreateEx = 0x5553EC;

enum Kind { kDD7, kSurf7, kD3D7, kDev7, kVB7, kGamma, kClipper, kPalette, kKinds };

const char* const kDD7Names[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
    "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces",
    "FlipToGDISurface", "GetCaps", "GetDisplayMode", "GetFourCCCodes", "GetGDISurface",
    "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", "Initialize",
    "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier", "StartModeTest", "EvaluateMode"};
const char* const kSurf7Names[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt",
    "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces",
    "EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps",
    "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette",
    "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock", "ReleaseDC", "Restore",
    "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay",
    "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface", "PageLock", "PageUnlock",
    "SetSurfaceDesc", "SetPrivateData", "GetPrivateData", "FreePrivateData",
    "GetUniquenessValue", "ChangeUniquenessValue", "SetPriority", "GetPriority", "SetLOD",
    "GetLOD"};
const char* const kD3D7Names[] = {"QueryInterface", "AddRef",          "Release",
                                  "EnumDevices",    "CreateDevice",    "CreateVertexBuffer",
                                  "EnumZBufferFormats", "EvictManagedTextures"};
const char* const kDev7Names[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "EnumTextureFormats", "BeginScene",
    "EndScene", "GetDirect3D", "SetRenderTarget", "GetRenderTarget", "Clear", "SetTransform",
    "GetTransform", "SetViewport", "MultiplyTransform", "GetViewport", "SetMaterial",
    "GetMaterial", "SetLight", "GetLight", "SetRenderState", "GetRenderState",
    "BeginStateBlock", "EndStateBlock", "PreLoad", "DrawPrimitive", "DrawIndexedPrimitive",
    "SetClipStatus", "GetClipStatus", "DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided",
    "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice",
    "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "Load",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "GetInfo"};
const char* const kVB7Names[] = {"QueryInterface", "AddRef",          "Release",
                                 "Lock",           "Unlock",          "ProcessVertices",
                                 "GetVertexBufferDesc", "Optimize", "ProcessVerticesStrided"};
const char* const kGammaNames[] = {"QueryInterface", "AddRef", "Release", "GetGammaRamp",
                                   "SetGammaRamp"};
const char* const kClipperNames[] = {"QueryInterface", "AddRef",  "Release",
                                     "GetClipList",    "GetHWnd", "Initialize",
                                     "IsClipListChanged", "SetClipList", "SetHWnd"};
const char* const kPaletteNames[] = {"QueryInterface", "AddRef",     "Release", "GetCaps",
                                     "GetEntries",     "Initialize", "SetEntries"};

struct KindInfo {
  const char* name;
  const char* const* methods;
  int count;
};
const KindInfo kInfo[kKinds] = {
    {"IDirectDraw7", kDD7Names, 30},
    {"IDirectDrawSurface7", kSurf7Names, 49},
    {"IDirect3D7", kD3D7Names, 8},
    {"IDirect3DDevice7", kDev7Names, 49},
    {"IDirect3DVertexBuffer7", kVB7Names, 9},
    {"IDirectDrawGammaControl", kGammaNames, 5},
    {"IDirectDrawClipper", kClipperNames, 9},
    {"IDirectDrawPalette", kPaletteNames, 7},
};

// One patched vtable: the original entries, and call counters per slot.
struct Table {
  Kind kind;
  void* original[64];
  LONG counts[64];
};

std::recursive_mutex g_mutex;
std::map<void**, Table*> g_tables;  // keyed by vtable address
std::set<std::string> g_seen;       // de-duplicates parameter log lines
LONG g_flips = 0;

decltype(&DirectDrawCreateEx) g_create = nullptr;

// Log a parameter line only the first time it is seen.
void LogOnce(const char* fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_seen.insert(buf).second) patch::Log("dd %s", buf);
}

Table* TableOf(void* object) {
  void** vtable = *static_cast<void***>(object);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  auto it = g_tables.find(vtable);
  return it == g_tables.end() ? nullptr : it->second;
}

template <typename Fn>
Fn Orig(void* object, int slot) {
  return reinterpret_cast<Fn>(TableOf(object)->original[slot]);
}

void Count(void* object, int slot) {
  if (Table* t = TableOf(object)) InterlockedIncrement(&t->counts[slot]);
}

// Build "inc dword [counter]; jmp [original]" for one slot.
void* MakeThunk(LONG* counter, void** original) {
  static uint8_t* page = nullptr;
  static size_t used = 0;
  const size_t size = 12;
  if (!page || used + size > 4096) {
    page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    used = 0;
  }
  uint8_t* p = page + used;
  used += size;
  p[0] = 0xFF;
  p[1] = 0x05;
  std::memcpy(p + 2, &counter, 4);
  p[6] = 0xFF;
  p[7] = 0x25;
  std::memcpy(p + 8, &original, 4);
  return p;
}

void* Handler(Kind kind, int slot);

// Patch the vtable of object (an interface of the given kind), once.
void Track(void* object, Kind kind) {
  if (!object) return;
  void** vtable = *static_cast<void***>(object);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_tables.count(vtable)) return;
  auto* t = new Table{};
  t->kind = kind;
  const int n = kInfo[kind].count;
  std::memcpy(t->original, vtable, n * sizeof(void*));
  void* entries[64];
  for (int i = 0; i < n; ++i) {
    void* h = Handler(kind, i);
    entries[i] = h ? h : MakeThunk(&t->counts[i], &t->original[i]);
  }
  g_tables[vtable] = t;
  patch::Write(reinterpret_cast<uintptr_t>(vtable), entries, n * sizeof(void*));
  patch::Log("dd tracking %s vtable %p", kInfo[kind].name, static_cast<void*>(vtable));
}

void TrackByIid(REFIID riid, void* object) {
  if (riid == IID_IDirect3D7) Track(object, kD3D7);
  else if (riid == IID_IDirectDraw7) Track(object, kDD7);
  else if (riid == IID_IDirectDrawSurface7) Track(object, kSurf7);
  else if (riid == IID_IDirectDrawGammaControl) Track(object, kGamma);
  else {
    const GUID& g = riid;
    LogOnce("QueryInterface for other interface {%08lx-%04x-%04x}", g.Data1, g.Data2, g.Data3);
  }
}

std::string SurfaceDescText(const DDSURFACEDESC2* d) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "flags=%lx caps=%lx/%lx size=%lux%lu bpp=%lu pf=%lx fourcc=%lx mips=%lu "
                "backbuffers=%lu",
                d->dwFlags, d->ddsCaps.dwCaps, d->ddsCaps.dwCaps2,
                (d->dwFlags & DDSD_WIDTH) ? d->dwWidth : 0,
                (d->dwFlags & DDSD_HEIGHT) ? d->dwHeight : 0,
                (d->dwFlags & DDSD_PIXELFORMAT) ? d->ddpfPixelFormat.dwRGBBitCount : 0,
                (d->dwFlags & DDSD_PIXELFORMAT) ? d->ddpfPixelFormat.dwFlags : 0,
                (d->dwFlags & DDSD_PIXELFORMAT) ? d->ddpfPixelFormat.dwFourCC : 0,
                (d->dwFlags & DDSD_MIPMAPCOUNT) ? d->dwMipMapCount : 0,
                (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0);
  return buf;
}

DWORD SurfaceCaps(IDirectDrawSurface7* s) {
  DDSCAPS2 caps{};
  if (Table* t = TableOf(s))
    reinterpret_cast<HRESULT(WINAPI*)(IDirectDrawSurface7*, DDSCAPS2*)>(t->original[14])(s,
                                                                                         &caps);
  return caps.dwCaps;
}

// ---- IDirectDraw7 handlers ----
HRESULT WINAPI DD7_QueryInterface(IDirectDraw7* self, REFIID riid, void** out) {
  Count(self, 0);
  HRESULT hr = Orig<HRESULT(WINAPI*)(IDirectDraw7*, REFIID, void**)>(self, 0)(self, riid, out);
  if (SUCCEEDED(hr) && out) TrackByIid(riid, *out);
  return hr;
}
HRESULT WINAPI DD7_CreateClipper(IDirectDraw7* self, DWORD f, LPDIRECTDRAWCLIPPER* out,
                                 IUnknown* u) {
  Count(self, 4);
  HRESULT hr = Orig<decltype(&DD7_CreateClipper)>(self, 4)(self, f, out, u);
  if (SUCCEEDED(hr) && out) Track(*out, kClipper);
  return hr;
}
HRESULT WINAPI DD7_CreatePalette(IDirectDraw7* self, DWORD f, LPPALETTEENTRY e,
                                 LPDIRECTDRAWPALETTE* out, IUnknown* u) {
  Count(self, 5);
  LogOnce("CreatePalette flags=%lx", f);
  HRESULT hr = Orig<decltype(&DD7_CreatePalette)>(self, 5)(self, f, e, out, u);
  if (SUCCEEDED(hr) && out) Track(*out, kPalette);
  return hr;
}
HRESULT WINAPI DD7_CreateSurface(IDirectDraw7* self, LPDDSURFACEDESC2 d,
                                 LPDIRECTDRAWSURFACE7* out, IUnknown* u) {
  Count(self, 6);
  HRESULT hr = Orig<decltype(&DD7_CreateSurface)>(self, 6)(self, d, out, u);
  if (d) LogOnce("CreateSurface %s -> %08lx", SurfaceDescText(d).c_str(), hr);
  if (SUCCEEDED(hr) && out) Track(*out, kSurf7);
  return hr;
}
HRESULT WINAPI DD7_SetCooperativeLevel(IDirectDraw7* self, HWND w, DWORD f) {
  Count(self, 20);
  HRESULT hr = Orig<decltype(&DD7_SetCooperativeLevel)>(self, 20)(self, w, f);
  LogOnce("SetCooperativeLevel flags=%lx -> %08lx", f, hr);
  return hr;
}
HRESULT WINAPI DD7_SetDisplayMode(IDirectDraw7* self, DWORD w, DWORD h, DWORD bpp, DWORD hz,
                                  DWORD f) {
  Count(self, 21);
  HRESULT hr = Orig<decltype(&DD7_SetDisplayMode)>(self, 21)(self, w, h, bpp, hz, f);
  patch::Log("dd SetDisplayMode %lux%lux%lu %luHz flags=%lx -> %08lx", w, h, bpp, hz, f, hr);
  return hr;
}

// ---- IDirectDrawSurface7 handlers ----
HRESULT WINAPI S7_QueryInterface(IDirectDrawSurface7* self, REFIID riid, void** out) {
  Count(self, 0);
  HRESULT hr =
      Orig<HRESULT(WINAPI*)(IDirectDrawSurface7*, REFIID, void**)>(self, 0)(self, riid, out);
  if (SUCCEEDED(hr) && out) TrackByIid(riid, *out);
  return hr;
}
HRESULT WINAPI S7_Blt(IDirectDrawSurface7* self, LPRECT dr, LPDIRECTDRAWSURFACE7 src, LPRECT sr,
                      DWORD f, LPDDBLTFX fx) {
  Count(self, 5);
  HRESULT hr = Orig<decltype(&S7_Blt)>(self, 5)(self, dr, src, sr, f, fx);
  LogOnce("Blt dstcaps=%lx srccaps=%lx flags=%lx -> %08lx", SurfaceCaps(self),
          src ? SurfaceCaps(src) : 0, f, hr);
  return hr;
}
HRESULT WINAPI S7_BltFast(IDirectDrawSurface7* self, DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src,
                          LPRECT sr, DWORD f) {
  Count(self, 7);
  HRESULT hr = Orig<decltype(&S7_BltFast)>(self, 7)(self, x, y, src, sr, f);
  LogOnce("BltFast dstcaps=%lx srccaps=%lx flags=%lx -> %08lx", SurfaceCaps(self),
          src ? SurfaceCaps(src) : 0, f, hr);
  return hr;
}
HRESULT WINAPI S7_Flip(IDirectDrawSurface7* self, LPDIRECTDRAWSURFACE7 o, DWORD f);
HRESULT WINAPI S7_GetAttachedSurface(IDirectDrawSurface7* self, LPDDSCAPS2 caps,
                                     LPDIRECTDRAWSURFACE7* out) {
  Count(self, 12);
  HRESULT hr = Orig<decltype(&S7_GetAttachedSurface)>(self, 12)(self, caps, out);
  if (caps) LogOnce("GetAttachedSurface caps=%lx -> %08lx", caps->dwCaps, hr);
  if (SUCCEEDED(hr) && out) Track(*out, kSurf7);
  return hr;
}
HRESULT WINAPI S7_GetDC(IDirectDrawSurface7* self, HDC* dc) {
  Count(self, 17);
  HRESULT hr = Orig<decltype(&S7_GetDC)>(self, 17)(self, dc);
  LogOnce("GetDC caps=%lx -> %08lx", SurfaceCaps(self), hr);
  return hr;
}
HRESULT WINAPI S7_Lock(IDirectDrawSurface7* self, LPRECT r, LPDDSURFACEDESC2 d, DWORD f,
                       HANDLE h) {
  Count(self, 25);
  HRESULT hr = Orig<decltype(&S7_Lock)>(self, 25)(self, r, d, f, h);
  LogOnce("Lock caps=%lx flags=%lx rect=%d bpp=%lu -> %08lx", SurfaceCaps(self), f, r ? 1 : 0,
          (SUCCEEDED(hr) && d) ? d->ddpfPixelFormat.dwRGBBitCount : 0, hr);
  return hr;
}
HRESULT WINAPI S7_SetColorKey(IDirectDrawSurface7* self, DWORD f, LPDDCOLORKEY k) {
  Count(self, 29);
  HRESULT hr = Orig<decltype(&S7_SetColorKey)>(self, 29)(self, f, k);
  LogOnce("SetColorKey caps=%lx flags=%lx -> %08lx", SurfaceCaps(self), f, hr);
  return hr;
}

// ---- IDirect3D7 handlers ----
HRESULT WINAPI D3D7_CreateDevice(IDirect3D7* self, REFCLSID cls, LPDIRECTDRAWSURFACE7 rt,
                                 LPDIRECT3DDEVICE7* out) {
  Count(self, 4);
  HRESULT hr = Orig<decltype(&D3D7_CreateDevice)>(self, 4)(self, cls, rt, out);
  const GUID& g = cls;
  patch::Log("dd CreateDevice {%08lx-%04x-%04x} rtcaps=%lx -> %08lx", g.Data1, g.Data2, g.Data3,
             rt ? SurfaceCaps(rt) : 0, hr);
  if (SUCCEEDED(hr) && out) Track(*out, kDev7);
  return hr;
}
HRESULT WINAPI D3D7_CreateVertexBuffer(IDirect3D7* self, LPD3DVERTEXBUFFERDESC d,
                                       LPDIRECT3DVERTEXBUFFER7* out, DWORD f) {
  Count(self, 5);
  HRESULT hr = Orig<decltype(&D3D7_CreateVertexBuffer)>(self, 5)(self, d, out, f);
  if (d)
    LogOnce("CreateVertexBuffer caps=%lx fvf=%lx n=%lu -> %08lx", d->dwCaps, d->dwFVF,
            d->dwNumVertices, hr);
  if (SUCCEEDED(hr) && out) Track(*out, kVB7);
  return hr;
}

// ---- IDirect3DDevice7 handlers ----
HRESULT WINAPI Dev_GetRenderTarget(IDirect3DDevice7* self, LPDIRECTDRAWSURFACE7* out) {
  Count(self, 9);
  HRESULT hr = Orig<decltype(&Dev_GetRenderTarget)>(self, 9)(self, out);
  if (SUCCEEDED(hr) && out) Track(*out, kSurf7);
  return hr;
}
HRESULT WINAPI Dev_Clear(IDirect3DDevice7* self, DWORD n, LPD3DRECT r, DWORD f, D3DCOLOR c,
                         D3DVALUE z, DWORD s) {
  Count(self, 10);
  LogOnce("Clear flags=%lx rects=%lu", f, n);
  return Orig<decltype(&Dev_Clear)>(self, 10)(self, n, r, f, c, z, s);
}
HRESULT WINAPI Dev_SetTransform(IDirect3DDevice7* self, D3DTRANSFORMSTATETYPE t,
                                LPD3DMATRIX m) {
  Count(self, 11);
  LogOnce("SetTransform type=%d", static_cast<int>(t));
  return Orig<decltype(&Dev_SetTransform)>(self, 11)(self, t, m);
}
HRESULT WINAPI Dev_SetViewport(IDirect3DDevice7* self, LPD3DVIEWPORT7 v) {
  Count(self, 13);
  if (v) LogOnce("SetViewport %lux%lu+%lu+%lu", v->dwWidth, v->dwHeight, v->dwX, v->dwY);
  return Orig<decltype(&Dev_SetViewport)>(self, 13)(self, v);
}
HRESULT WINAPI Dev_SetRenderState(IDirect3DDevice7* self, D3DRENDERSTATETYPE s, DWORD v) {
  Count(self, 20);
  LogOnce("SetRenderState %d = %lx", static_cast<int>(s), v);
  return Orig<decltype(&Dev_SetRenderState)>(self, 20)(self, s, v);
}
HRESULT WINAPI Dev_DrawPrimitive(IDirect3DDevice7* self, D3DPRIMITIVETYPE p, DWORD fvf, LPVOID v,
                                 DWORD n, DWORD f) {
  Count(self, 25);
  LogOnce("DrawPrimitive prim=%d fvf=%lx flags=%lx", static_cast<int>(p), fvf, f);
  return Orig<decltype(&Dev_DrawPrimitive)>(self, 25)(self, p, fvf, v, n, f);
}
HRESULT WINAPI Dev_DrawIndexedPrimitive(IDirect3DDevice7* self, D3DPRIMITIVETYPE p, DWORD fvf,
                                        LPVOID v, DWORD nv, LPWORD i, DWORD ni, DWORD f) {
  Count(self, 26);
  LogOnce("DrawIndexedPrimitive prim=%d fvf=%lx flags=%lx", static_cast<int>(p), fvf, f);
  return Orig<decltype(&Dev_DrawIndexedPrimitive)>(self, 26)(self, p, fvf, v, nv, i, ni, f);
}
HRESULT WINAPI Dev_DrawPrimitiveVB(IDirect3DDevice7* self, D3DPRIMITIVETYPE p,
                                   LPDIRECT3DVERTEXBUFFER7 vb, DWORD s, DWORD n, DWORD f) {
  Count(self, 31);
  LogOnce("DrawPrimitiveVB prim=%d flags=%lx", static_cast<int>(p), f);
  return Orig<decltype(&Dev_DrawPrimitiveVB)>(self, 31)(self, p, vb, s, n, f);
}
HRESULT WINAPI Dev_DrawIndexedPrimitiveVB(IDirect3DDevice7* self, D3DPRIMITIVETYPE p,
                                          LPDIRECT3DVERTEXBUFFER7 vb, DWORD s, DWORD nv,
                                          LPWORD i, DWORD ni, DWORD f) {
  Count(self, 32);
  LogOnce("DrawIndexedPrimitiveVB prim=%d flags=%lx", static_cast<int>(p), f);
  return Orig<decltype(&Dev_DrawIndexedPrimitiveVB)>(self, 32)(self, p, vb, s, nv, i, ni, f);
}
HRESULT WINAPI Dev_GetTexture(IDirect3DDevice7* self, DWORD st, LPDIRECTDRAWSURFACE7* out) {
  Count(self, 34);
  HRESULT hr = Orig<decltype(&Dev_GetTexture)>(self, 34)(self, st, out);
  if (SUCCEEDED(hr) && out && *out) Track(*out, kSurf7);
  return hr;
}
HRESULT WINAPI Dev_SetTexture(IDirect3DDevice7* self, DWORD st, LPDIRECTDRAWSURFACE7 t) {
  Count(self, 35);
  LogOnce("SetTexture stage=%lu", st);
  return Orig<decltype(&Dev_SetTexture)>(self, 35)(self, st, t);
}
HRESULT WINAPI Dev_SetTextureStageState(IDirect3DDevice7* self, DWORD st,
                                        D3DTEXTURESTAGESTATETYPE t, DWORD v) {
  Count(self, 37);
  LogOnce("SetTextureStageState stage=%lu %d = %lx", st, static_cast<int>(t), v);
  return Orig<decltype(&Dev_SetTextureStageState)>(self, 37)(self, st, t, v);
}

void DumpCounts(const char* why) {
  // At process exit other threads are gone and may have died holding the
  // lock, so never wait for it.
  std::unique_lock<std::recursive_mutex> lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  patch::Log("dd ---- call counts (%s) ----", why);
  for (auto& [vtable, t] : g_tables) {
    const KindInfo& k = kInfo[t->kind];
    for (int i = 0; i < k.count; ++i)
      if (t->counts[i]) patch::Log("dd count %s::%s %ld", k.name, k.methods[i], t->counts[i]);
  }
}

HRESULT WINAPI S7_Flip(IDirectDrawSurface7* self, LPDIRECTDRAWSURFACE7 o, DWORD f) {
  Count(self, 11);
  LogOnce("Flip flags=%lx", f);
  HRESULT hr = Orig<decltype(&S7_Flip)>(self, 11)(self, o, f);
  if (FAILED(hr)) LogOnce("Flip failed %08lx", hr);
  if ((InterlockedIncrement(&g_flips) & 4095) == 0) DumpCounts("periodic");
  return hr;
}

void* Handler(Kind kind, int slot) {
  switch (kind) {
    case kDD7:
      switch (slot) {
        case 0: return reinterpret_cast<void*>(&DD7_QueryInterface);
        case 4: return reinterpret_cast<void*>(&DD7_CreateClipper);
        case 5: return reinterpret_cast<void*>(&DD7_CreatePalette);
        case 6: return reinterpret_cast<void*>(&DD7_CreateSurface);
        case 20: return reinterpret_cast<void*>(&DD7_SetCooperativeLevel);
        case 21: return reinterpret_cast<void*>(&DD7_SetDisplayMode);
      }
      break;
    case kSurf7:
      switch (slot) {
        case 0: return reinterpret_cast<void*>(&S7_QueryInterface);
        case 5: return reinterpret_cast<void*>(&S7_Blt);
        case 7: return reinterpret_cast<void*>(&S7_BltFast);
        case 11: return reinterpret_cast<void*>(&S7_Flip);
        case 12: return reinterpret_cast<void*>(&S7_GetAttachedSurface);
        case 17: return reinterpret_cast<void*>(&S7_GetDC);
        case 25: return reinterpret_cast<void*>(&S7_Lock);
        case 29: return reinterpret_cast<void*>(&S7_SetColorKey);
      }
      break;
    case kD3D7:
      switch (slot) {
        case 4: return reinterpret_cast<void*>(&D3D7_CreateDevice);
        case 5: return reinterpret_cast<void*>(&D3D7_CreateVertexBuffer);
      }
      break;
    case kDev7:
      switch (slot) {
        case 9: return reinterpret_cast<void*>(&Dev_GetRenderTarget);
        case 10: return reinterpret_cast<void*>(&Dev_Clear);
        case 11: return reinterpret_cast<void*>(&Dev_SetTransform);
        case 13: return reinterpret_cast<void*>(&Dev_SetViewport);
        case 20: return reinterpret_cast<void*>(&Dev_SetRenderState);
        case 25: return reinterpret_cast<void*>(&Dev_DrawPrimitive);
        case 26: return reinterpret_cast<void*>(&Dev_DrawIndexedPrimitive);
        case 31: return reinterpret_cast<void*>(&Dev_DrawPrimitiveVB);
        case 32: return reinterpret_cast<void*>(&Dev_DrawIndexedPrimitiveVB);
        case 34: return reinterpret_cast<void*>(&Dev_GetTexture);
        case 35: return reinterpret_cast<void*>(&Dev_SetTexture);
        case 37: return reinterpret_cast<void*>(&Dev_SetTextureStageState);
      }
      break;
    default:
      break;
  }
  return nullptr;
}

HRESULT WINAPI HookedDirectDrawCreateEx(GUID* guid, LPVOID* out, REFIID riid, IUnknown* outer) {
  HRESULT hr = g_create(guid, out, riid, outer);
  patch::Log("dd DirectDrawCreateEx guid=%s -> %08lx", guid ? "device" : "primary", hr);
  if (SUCCEEDED(hr) && out) TrackByIid(riid, *out);
  return hr;
}

}  // namespace

void ApplyDirectDrawTrace() {
  g_create = reinterpret_cast<decltype(&DirectDrawCreateEx)>(patch::HookIat(
      kIatDirectDrawCreateEx, reinterpret_cast<void*>(&HookedDirectDrawCreateEx)));
  patch::Log("dd trace: on");
}

void DumpDirectDrawTrace() { DumpCounts("exit"); }
