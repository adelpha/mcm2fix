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

void ApplyD3D11Backend() {
  patch::HookIat(kIatDirectDrawCreateEx, reinterpret_cast<void*>(&dd::CreateDirectDraw));
  patch::Log("d3d11 backend: on");
  dd::InstallGdiHooks();
  dd::InstallHudTextHooks();
}
