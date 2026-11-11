// Startup simplifications requested for the Windows 11 build:
// no adapter picker and no "detected a change" box.
#include <windows.h>
#include <cstdint>

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

void ApplyStartupFixes() {
  ApplyNoDevicePicker();
  ApplyPrimaryAdapterOnly();
  ApplyNoHardwareChangeBox();
}
