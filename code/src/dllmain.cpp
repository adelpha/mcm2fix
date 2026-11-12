// mcm2fix.dll: loaded by Mcm2.exe in place of d3drm.dll. Applies the
// compatibility fixes to the game's code before its entry point runs.
#include <windows.h>
#include <cstdint>

#include "config.h"
#include "patch.h"
#include "prof.h"
#include "video/player.h"
#include "xp_compat.h"

void ApplyStep1Fixes();
void ApplyStartupFixes();
void ApplyRegistryFixes();
void ApplyFileFixes();
void ApplyDiagnostics();
void ApplyRegistryTrace();
void ApplyD3D11Backend();
void ApplyWidescreenFov();
void ApplyFramePacing();
void ApplyDetailFixes();
void ApplyArrowFix();
void ApplyRestartFixes();
void ApplyHelpFix();
void ApplyTextureFixes();
void ApplyPerfFixes();
void ApplySoundFixes();
void ApplySortFixes();
void ApplyAiFixes();
namespace input { void Apply(); }
void LogTextureSummary();
namespace crash { void Apply(); }

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
      xp::LogSystemInfo();
      crash::Apply();
      ApplyStep1Fixes();
      const Config& c = GetConfig();
      patch::Log("config: OneCore=%d Startup=%d Registry=%d Files=%d ActivateLog=%d RegLog=%d DDStats=%d DrawLog=%d DrawBatch=%d BatchLog=%d "
                 "AlphaCut=%d AlphaToCoverage=%d WideFov=%d ArrowFix=%d ArrowScale=%.2f RestartFix=%d RestartLog=%d",
                 c.one_core, c.startup, c.registry, c.files, c.activate_log, c.reg_log, c.dd_stats, c.draw_log, c.draw_batch, c.batch_log,
                 c.alpha_cut, c.alpha_to_coverage, c.wide_fov, c.arrow_heading, static_cast<double>(c.arrow_scale),
                 c.restart_fix, c.restart_log);
      patch::Log("display: Fullscreen=%s FullscreenMode=%dx%d@%d (0 = desktop) VSync=%d",
                 c.exclusive ? "Exclusive" : "Borderless", c.fs_width, c.fs_height, c.fs_hz, c.vsync ? 1 : 0);
      patch::Log("graphics: Fov=%.1f Anisotropy=%d Antialiasing=%d MipBias=%.2f FogDistance=%.2f "
                 "ViewDistance=%.2f ModelLod=%.2f TreeDetailDistance=%d TextureReplace=%d TextureDump=%d "
                 "Renderer=D3D%d RendererLabel=%d D3D9Log=%d",
                 c.fov, c.anisotropy, c.msaa, c.mip_bias, c.fog_scale, c.view_distance, c.model_lod, c.tree_always_near ? 0 : 1,
                 c.texture_replace, c.texture_dump, c.renderer, c.renderer_label, c.d3d9_log);
      if (c.startup) ApplyStartupFixes();
      if (c.registry) ApplyRegistryFixes();
      if (c.files) ApplyFileFixes();
      if (c.reg_log) ApplyRegistryTrace();
      ApplyD3D11Backend();
      ApplyWidescreenFov();
      ApplyFramePacing();
      ApplyDetailFixes();
      ApplyArrowFix();
      ApplyRestartFixes();
      ApplyHelpFix();
      ApplyTextureFixes();
      ApplyPerfFixes();
      ApplySoundFixes();
      ApplySortFixes();
      ApplyAiFixes();
      input::Apply();
      video::ApplyVideoPlayer();
      if (c.activate_log) ApplyDiagnostics();
      prof::Start();
    } else {
      patch::Log("not the supported Mcm2.exe; no fixes applied");
    }
  } else if (reason == DLL_PROCESS_DETACH) {
    prof::Stop();
    LogTextureSummary();
  }
  return TRUE;
}
