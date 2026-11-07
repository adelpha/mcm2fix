// mcm2fix.dll: loaded by Mcm2.exe in place of d3drm.dll. Applies the
// compatibility fixes to the game's code before its entry point runs.
#include <windows.h>
#include <cstdint>

#include "config.h"
#include "patch.h"

void ApplyStep1Fixes();
void ApplyStartupFixes();
void ApplyRegistryFixes();
void ApplyFileFixes();
void ApplyDiagnostics();
void ApplyRegistryTrace();
void ApplyD3D11Backend();
void ApplyWidescreenFov();

namespace {

// Make sure we are inside the Mcm2.exe build these addresses come from.
bool IsSupportedGame() {
  auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  if (base != 0x400000) return false;
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  return nt->OptionalHeader.AddressOfEntryPoint == 0x13D6F5 &&
         nt->OptionalHeader.SizeOfImage == 0x296000;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    LoadConfig();
    if (IsSupportedGame()) {
      patch::Log("mcm2fix loaded");
      ApplyStep1Fixes();
      const Config& c = GetConfig();
      patch::Log("config: OneCore=%d Startup=%d Registry=%d Files=%d ActivateLog=%d RegLog=%d DDStats=%d "
                 "AlphaCut=%d WideFov=%d",
                 c.one_core, c.startup, c.registry, c.files, c.activate_log, c.reg_log, c.dd_stats,
                 c.alpha_cut, c.wide_fov);
      if (c.startup) ApplyStartupFixes();
      if (c.registry) ApplyRegistryFixes();
      if (c.files) ApplyFileFixes();
      if (c.reg_log) ApplyRegistryTrace();
      ApplyD3D11Backend();
      ApplyWidescreenFov();
      if (c.activate_log) ApplyDiagnostics();
    } else {
      patch::Log("not the supported Mcm2.exe; no fixes applied");
    }
  }
  return TRUE;
}
