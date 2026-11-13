// Logging hooks used to track down the multi-core startup problems.
#include <windows.h>
#include <cstdint>
#include <mmsystem.h>

#include "config.h"
#include "framerate.h"
#include "input_timing.h"
#include "patch.h"

void SoundOnActivate(bool active);
bool PerfClockReplaced();
DWORD PerfRawMs();

DWORD FocusClock(DWORD raw_ms);

namespace {

constexpr uintptr_t kActiveFlag = 0x573F48;   // set from WM_ACTIVATEAPP wParam
constexpr uintptr_t kMainWindow = 0x68B16C;   // game window HWND

// WndProc, WM_ACTIVATEAPP: 4a23db  mov [573f48], edi  (edi = wParam).
// While this flag is 0 the main loop only pumps messages and never ticks.
// The game's clock (0x4c41d0, timeGetTime, 96 callers) stops while the
// game is in the background. The main loop doesn't tick then, but the clock
// ran on, so coming back the game's own timers found the whole time away
// due at once: the race stalled for seconds and then ran fast to catch up
// (StOo, 2026-11-13). Time away is now taken off the clock.
// Both clock replacements (ours here, or the QPC one in fixes_perf.cpp,
// which comes first) return FocusClock(raw): frozen while inactive, and the
// time away taken off afterwards, so it never runs backwards.
volatile DWORD g_away_ms = 0;      // total time spent inactive
volatile DWORD g_inactive_at = 0;  // raw clock when it went inactive (0 = active)
bool g_clock_patched = false;
bool g_perf_clock = false;         // fixes_perf.cpp replaced the clock

DWORD RawMs() { return g_perf_clock ? PerfRawMs() : timeGetTime(); }

extern "C" DWORD __cdecl GameClock() { return FocusClock(timeGetTime()); }

void ClockActivate(bool active) {
  if (!g_clock_patched) return;
  const DWORD now = RawMs();
  if (!active && !g_inactive_at) {
    g_inactive_at = now ? now : 1;
  } else if (active && g_inactive_at) {
    const DWORD away = now - g_inactive_at;
    g_away_ms += away;
    g_inactive_at = 0;
    patch::Log("focus: game clock held for the %.1f s the game was inactive (%.1f s in total)", away / 1000.0,
               g_away_ms / 1000.0);
  }
}

// The game hides the Windows cursor when it starts (0x4c53e0, ShowCursor
// until the count is below 0) and draws its own in menus. Its deactivate
// handler (0x4c4a60) shows it again only for flag +0x2d4 bit 1, which isn't
// set in our windowed modes, so over the inactive window the cursor stayed
// invisible (StOo, 2026-11-13). Shown while inactive, hidden again on return.
// The window class cursor is also blank.cur (an empty cursor, 0x4a21ed
// LoadCursorFromFileA), so while inactive the class gets the arrow instead.
int g_cursor_raised = 0;
HCURSOR g_class_cursor = nullptr;  // the game's, while we swapped in the arrow
bool g_class_swapped = false;

void SwapClassCursor(bool to_arrow) {
  HWND game = *reinterpret_cast<HWND*>(kMainWindow);
  if (!game) return;
  if (to_arrow && !g_class_swapped) {
    g_class_cursor = reinterpret_cast<HCURSOR>(
        SetClassLongPtrA(game, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(LoadCursor(nullptr, IDC_ARROW))));
    g_class_swapped = true;
    patch::Log("focus: window class cursor %p (the game's blank.cur) swapped for the arrow", g_class_cursor);
  } else if (!to_arrow && g_class_swapped) {
    SetClassLongPtrA(game, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(g_class_cursor));
    g_class_swapped = false;
  }
}

void CursorActivate(bool active) {
  if (!GetConfig().alt_tab_fix) return;
  SwapClassCursor(!active);
  if (!active) {
    if (g_cursor_raised) return;
    int c = ShowCursor(TRUE), n = 1;
    if (c > 0) {  // it was visible already: undo our call
      ShowCursor(FALSE);
      n = 0;
    }
    while (c < 0 && n < 64) {
      c = ShowCursor(TRUE);
      ++n;
    }
    g_cursor_raised = n;
    patch::Log("focus: cursor %s while the game is inactive", g_cursor_raised ? "shown" : "already visible");
  } else if (g_cursor_raised) {
    for (int i = 0; i < g_cursor_raised; ++i) ShowCursor(FALSE);
    g_cursor_raised = 0;
    patch::Log("focus: cursor hidden again (game in front)");
  }
}

// Also tells input (AltTabFix) and sound (BackgroundMute).
extern "C" void __cdecl LogActivateApp(DWORD active) {
  HWND game = *reinterpret_cast<HWND*>(kMainWindow);
  HWND fg = GetForegroundWindow();
  patch::Log("focus: WM_ACTIVATEAPP active=%lu game=%p foreground=%p%s", active, game, fg,
             fg == game ? " (game)" : "");
  ClockActivate(active != 0);
  CursorActivate(active != 0);
  // PauseOnFocusLoss: going to the background mid-race opens the pause menu,
  // as Escape does (WM_KEYDOWN reaches the race state through game vfunc
  // +0x90, 0x4c4ac0); the message loop still runs while inactive.
  if (!active && game && GetConfig().pause_on_focus_loss && SimRacing()) {
    const BOOL ok = PostMessageA(game, WM_KEYDOWN, VK_ESCAPE, 0x00010001) &&
                    PostMessageA(game, WM_KEYUP, VK_ESCAPE, 0xC0010001);
    patch::Log("focus: race running, Escape sent to open the pause menu (%s)", ok ? "posted" : "post FAILED");
  }
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

DWORD FocusClock(DWORD raw_ms) {
  const DWORD at = g_inactive_at;
  return (at ? at : raw_ms) - g_away_ms;
}

void ApplyDiagnostics() {
  static const uint8_t original[] = {0x89, 0x3d, 0x48, 0x3f, 0x57, 0x00};
  if (!patch::Matches(0x4A23DB, original, sizeof(original))) {
    patch::Log("diag: activate hook skipped");
    return;
  }
  patch::Call(0x4A23DB, reinterpret_cast<const void*>(&ActivateThunk), sizeof(original));
  // 4c41d0: push esi; push 1; call [timeBeginPeriod]; call [timeGetTime] ...
  static const uint8_t clock[] = {0x56, 0x6a, 0x01, 0xff, 0x15, 0x98, 0x53, 0x55, 0x00, 0xff, 0x15, 0xa4, 0x53, 0x55, 0x00};
  if (PerfClockReplaced()) {
    g_perf_clock = true;  // fixes_perf.cpp's QPC clock calls FocusClock
    g_clock_patched = true;
  } else if (patch::Matches(0x4C41D0, clock, sizeof(clock))) {
    timeBeginPeriod(1);  // the original wrapped each read in timeBeginPeriod(1)/timeEndPeriod(1)
    patch::Jump(0x4C41D0, reinterpret_cast<uintptr_t>(&GameClock), 5);
    g_clock_patched = true;
  }
  patch::Log("focus: game clock %s", g_clock_patched ? "stops while the game is inactive" : "NOT patched (code differs)");
  (void)kActiveFlag;
}
