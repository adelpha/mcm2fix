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

}  // namespace

void ApplyStartupFixes() {
  ApplyPrimaryAdapterOnly();
  ApplyNoHardwareChangeBox();
}
