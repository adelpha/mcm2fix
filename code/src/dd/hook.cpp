// Installs the D3D11 backend: Mcm2.exe's DirectDrawCreateEx import is pointed
// at mcm2fix's own DirectDraw implementation.
#include <windows.h>

#include "../patch.h"
#include "backend.h"
#include "directdraw.h"
#include "gdihook.h"
#include "hudtext.h"

namespace {

constexpr uintptr_t kIatDirectDrawCreateEx = 0x5553EC;

}  // namespace

// Direct3D 11 needs Vista SP2 / 7 or later. Without it (Windows XP) the
// game keeps Windows' own DirectDraw renderer, which XP still has, until the
// Direct3D 9 renderer is available.
bool D3D11Available() {
  HMODULE d3d = LoadLibraryA("d3d11.dll");
  HMODULE dxgi = LoadLibraryA("dxgi.dll");
  const bool ok = d3d && dxgi && GetProcAddress(d3d, "D3D11CreateDevice") &&
                  GetProcAddress(dxgi, "CreateDXGIFactory1");
  patch::Log("d3d11 backend: d3d11.dll %s, dxgi.dll %s%s", d3d ? "loaded" : "missing",
             dxgi ? "loaded" : "missing", ok ? "" : " (entry points missing)");
  return ok;
}

// The Direct3D 9 renderer (fallback when D3D11 is missing) needs d3d9.dll.
bool D3D9Available() {
  HMODULE d3d9 = LoadLibraryA("d3d9.dll");
  const bool ok = d3d9 && GetProcAddress(d3d9, "Direct3DCreate9");
  patch::Log("d3d11 backend: d3d9.dll %s", ok ? "available" : "missing");
  return ok;
}

void ApplyD3D11Backend() {
  if (!D3D11Available() && !D3D9Available()) {
    patch::Log("d3d11 backend: off, neither Direct3D 11 nor Direct3D 9 available; using Windows' "
               "own DirectDraw (exclusive fullscreen, no widescreen, no graphics options)");
    return;
  }
  patch::HookIat(kIatDirectDrawCreateEx, reinterpret_cast<void*>(&dd::CreateDirectDraw));
  patch::Log("d3d11 backend: on");
  dd::InstallGdiHooks();
  dd::InstallHudTextHooks();
}
