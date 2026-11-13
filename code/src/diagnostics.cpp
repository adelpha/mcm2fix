// Logging hooks used to track down the multi-core startup problems.
#include <windows.h>
#include <cstdint>

#include "config.h"
#include "input_timing.h"
#include "patch.h"

void SoundOnActivate(bool active);

namespace {

constexpr uintptr_t kActiveFlag = 0x573F48;   // set from WM_ACTIVATEAPP wParam
constexpr uintptr_t kMainWindow = 0x68B16C;   // game window HWND

// WndProc, WM_ACTIVATEAPP: 4a23db  mov [573f48], edi  (edi = wParam).
// While this flag is 0 the main loop only pumps messages and never ticks.
// Also tells input (AltTabFix) and sound (BackgroundMute).
extern "C" void __cdecl LogActivateApp(DWORD active) {
  HWND game = *reinterpret_cast<HWND*>(kMainWindow);
  HWND fg = GetForegroundWindow();
  patch::Log("focus: WM_ACTIVATEAPP active=%lu game=%p foreground=%p%s", active, game, fg,
             fg == game ? " (game)" : "");
  input::OnActivate(active != 0);
  SoundOnActivate(active != 0);
}

#if defined(_MSC_VER)
__declspec(naked) void ActivateThunk() {
  __asm {
    pushad
    push edi
    call LogActivateApp
    add esp, 4
    popad
    mov dword ptr ds:[0x573F48], edi
    ret
  }
}
#else
extern "C" void ActivateThunk();
asm(".text\n"
    ".globl _ActivateThunk\n"
    "_ActivateThunk:\n"
    "  pushal\n"
    "  pushl %edi\n"
    "  call _LogActivateApp\n"
    "  addl $4, %esp\n"
    "  popal\n"
    "  movl %edi, 0x573F48\n"
    "  ret\n");
#endif

}  // namespace

void ApplyDiagnostics() {
  static const uint8_t original[] = {0x89, 0x3d, 0x48, 0x3f, 0x57, 0x00};
  if (!patch::Matches(0x4A23DB, original, sizeof(original))) {
    patch::Log("diag: activate hook skipped");
    return;
  }
  patch::Call(0x4A23DB, reinterpret_cast<const void*>(&ActivateThunk), sizeof(original));
  (void)kActiveFlag;
}
