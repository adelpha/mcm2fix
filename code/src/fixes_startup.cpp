// Startup simplifications requested for the Windows 11 build:
// no adapter picker and no "detected a change" box.
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "patch.h"

namespace {

// 4cdd50 enumerates DirectDraw devices with
// DirectDrawEnumerateExA(callback, ctx, DDENUM_ATTACHEDSECONDARYDEVICES |
// DDENUM_NONDISPLAYDEVICES), which lists the primary display driver plus one
// device per monitor. With flags 0 only the primary display driver is
// listed, so the renderer picker (4d14b0) auto-selects it and never opens,
// and the hardware profiler only tests (and mode-switches) one device.
void ApplyPrimaryAdapterOnly() {
  static const uint8_t original[] = {0x6a, 0x05};  // push 5
  static const uint8_t replacement[] = {0x6a, 0x00};  // push 0
  patch::Replace("primary-adapter-only", 0x4CDD78, original, sizeof(original), replacement,
                 sizeof(replacement));
}

// 4c46ba: after detecting new hardware the game shows "Motocross Madness 2
// has detected a change in your game, video hardware or driver installation"
// with OK/Cancel before re-profiling. Skip the box and carry on as if OK.
void ApplyNoHardwareChangeBox() {
  static const uint8_t original[] = {0x74, 0x43};     // je 4c46ff (string missing)
  static const uint8_t replacement[] = {0xeb, 0x43};  // jmp 4c46ff
  patch::Replace("no-hardware-change-box", 0x4C46BA, original, sizeof(original), replacement,
                 sizeof(replacement));
}

// 4d14b0 (cdecl, 3 args) returns the renderer device to use, or opens the
// "choose a video card" window when the count of usable hardware devices is
// not exactly one or the game asks for it. The software renderer (Blade) is
// also in the list. Replace it: return the first hardware device (flag bit
// 0 at +0x1b8), else the first device. The device list is at 690894 (4
// pointers) with its count at 6908a4; each device's name is at +0x4bc.
// The window writes the third argument (game+0x2d0): 1 when the "Software
// Renderer" entry was chosen, 0 for a hardware card. Always choose hardware.
void* __cdecl PickRenderDevice(int, int, void* use_software) {
  auto** devices = reinterpret_cast<uint8_t**>(0x690894);
  int count = *reinterpret_cast<int*>(0x6908A4);
  if (count > 4) count = 4;
  void* pick = nullptr;
  for (int i = 0; i < count; ++i) {
    uint8_t* d = devices[i];
    if (!d) continue;
    bool hardware = (d[0x1B8] & 1) != 0;
    patch::Log("device %d: \"%.64s\" hardware=%d", i, reinterpret_cast<char*>(d + 0x4BC),
               hardware ? 1 : 0);
    if (!pick && hardware) pick = d;
  }
  if (!pick && count > 0) pick = devices[0];
  if (use_software) {
    auto* flag = static_cast<int*>(use_software);
    patch::Log("software-renderer flag was %d, set to 0", *flag);
    *flag = 0;
  }
  patch::Log("picked device %p", pick);
  return pick;
}

void ApplyNoDevicePicker() {
  static const uint8_t original[] = {0x81, 0xec, 0x0c, 0x03, 0x00, 0x00};  // sub esp, 0x30c
  if (!patch::Matches(0x4D14B0, original, sizeof(original))) {
    patch::Log("no-device-picker: unexpected bytes, skipped");
    return;
  }
  patch::Jump(0x4D14B0, reinterpret_cast<uintptr_t>(&PickRenderDevice), sizeof(original));
  patch::Log("no-device-picker: applied");
}

}  // namespace

// 526260: the licence check loads EBUEula.dll and calls EBUEula(reg key,
// "EULA.rtf", 0, 1), which shows the licence once and remembers the answer.
// Both files sit in the CD root, outside the GAME folder Setup copies from,
// and Setup copies them separately: a game copied off the CD by hand lacks
// them, and then the game quits silently at startup (LoadLibrary fails, or
// the DLL can't open EULA.rtf and answers "declined"). When either file is
// missing next to Mcm2.exe the check is skipped and the game carries on as
// if the licence was accepted (526336). With both present it runs as before.
static void ApplyEulaSkip() {
  char dir[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
  while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') --n;
  dir[n] = 0;
  bool missing[2] = {};
  const char* names[2] = {"EBUEula.dll", "EULA.rtf"};
  for (int i = 0; i < 2; ++i) {
    char path[MAX_PATH + 16];
    lstrcpynA(path, dir, MAX_PATH);
    lstrcatA(path, names[i]);
    missing[i] = GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
  }
  if (!missing[0] && !missing[1]) {
    patch::Log("eula: EBUEula.dll and EULA.rtf found; the game's licence check runs as normal");
    return;
  }
  // push 57b7b4 ("EBUEula.dll"); call [LoadLibraryA] -> jmp 526336 (accepted)
  static const uint8_t original[] = {0x68, 0xb4, 0xb7, 0x57, 0x00, 0xff, 0x15, 0x6c, 0x52, 0x55, 0x00};
  uint8_t replacement[sizeof(original)];
  const int32_t rel = 0x526336 - (0x526260 + 5);
  replacement[0] = 0xe9;
  memcpy(replacement + 1, &rel, 4);
  for (size_t i = 5; i < sizeof(replacement); ++i) replacement[i] = 0x90;
  const bool ok = patch::Replace("eula-skip", 0x526260, original, sizeof(original), replacement, sizeof(replacement));
  patch::Log("eula: %s%s%s missing from %s; licence check %s", missing[0] ? "EBUEula.dll" : "",
             missing[0] && missing[1] ? " and " : "", missing[1] ? "EULA.rtf" : "", dir,
             ok ? "skipped (treated as accepted)" : "NOT skipped (code differs): the game may quit at startup");
}

// The game isn't DPI aware, so with Windows display scaling above 100 %
// Windows runs it at a shrunken virtual size and stretches the picture: blurry,
// widescreen modes capped at the shrunken size, and DirectInput's mouse gets
// a constant drift (slow moves go the wrong way in menus). Declared here,
// before the game creates its window. Looked up at run time (XP has none).
void ApplyDpiAwareness() {
  HMODULE user32 = GetModuleHandleA("user32.dll");
  const char* how = "none available (Windows XP?)";
  bool ok = false;
  using CtxFn = BOOL(WINAPI*)(HANDLE);
  using AwareFn = BOOL(WINAPI*)();
  using ShcoreFn = HRESULT(WINAPI*)(int);
  if (auto ctx = user32 ? reinterpret_cast<CtxFn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) : nullptr) {
    how = "per-monitor v2";
    ok = ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (!ok) {
      how = "per-monitor";
      ok = ctx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-3)));
    }
  }
  if (!ok) {
    if (HMODULE shcore = GetModuleHandleA("shcore.dll")) {  // no LoadLibrary under the loader lock
      if (auto f = reinterpret_cast<ShcoreFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")))) {
        how = "per-monitor (shcore)";
        ok = SUCCEEDED(f(2));
      }
    }
  }
  if (!ok && user32) {
    if (auto f = reinterpret_cast<AwareFn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDPIAware")))) {
      how = "system";
      ok = f() != FALSE;
    }
  }
  HDC dc = GetDC(nullptr);
  const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 0;
  if (dc) ReleaseDC(nullptr, dc);
  patch::Log("dpi: DPI awareness %s: %s (error %lu); screen %dx%d, %d dpi (%d %% scaling)", how, ok ? "set" : "NOT set",
             ok ? 0UL : GetLastError(), GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), dpi,
             dpi ? dpi * 100 / 96 : 0);
}

void ApplyStartupFixes() {
  ApplyEulaSkip();
  ApplyNoDevicePicker();
  ApplyPrimaryAdapterOnly();
  ApplyNoHardwareChangeBox();
}
