// Resolution: mcm2max.ini decides the game's resolution, not the game.
//
// The game saves its resolution as a position in its list of display modes
// (options +0xfc4, the Options dropdown position) and starts at 640x480. Its
// list comes from EnumDisplayModes, i.e. from display.cpp (the monitor's own
// modes: sizes up to the desktop with Fullscreen=Borderless, every size and
// refresh rate with Fullscreen=Exclusive).
//
// The wanted mode is BorderlessResolution= or ExclusiveResolution= (each
// mode keeps its own), Desktop until the player picks one. Every frame the
// game's saved position is pointed at that mode; a pick in Options > Graphics
// is written back to the ini as its size (and refresh rate). A saved mode the
// monitor no longer has falls back to the desktop for that start; the ini is
// left as it is.
//
// The game shows its menus at 640x480 and switches to this mode for races;
// those switches stay virtual. With Fullscreen=Exclusive the screen is set to
// this mode the whole time (dd/backend*.cpp, ResolutionWanted), menus scaled.
//
// Exe (2008 no-CD Mcm2.exe):
//   [0x5743ac] = game; game+0xfc4 = saved mode position;
//   [game+0xc] = display card: +0x8 mode count, +0x10 mode array of
//   0x24-byte records {width, height, bpp, refresh, ...}, filled by the
//   EnumDisplayModes callback at 0x4cddb0.
//   Options > Graphics save (OptGraph exchange 0x4b6d80):
//     0x4b6da3 mov [0x68e918],eax, eax = the dropdown's position.
//   Another dialog's OK at 0x453846 does the same.
//   Options > Graphics fills the dropdown at 0x4b6b98: sprintf "%d X %d"
//   at 0x4b6bdb / 0x4b6c2c with esi = mode position.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "menugame.h"
#include "menunav.h"
#include "config.h"
#include "display.h"
#include "patch.h"
#include "dd/backend.h"
#include "dd/gputext.h"
#include "mode_prompt.h"

namespace {

int g_want_w = 0, g_want_h = 0, g_want_hz = 0;  // the mode the game should use (never 0 after Init)
bool g_want_desktop = true;  // ...because the ini says Desktop (the dropdown shows "Desktop")
// The dropdown's "Desktop" entry: item data the game's mode positions never reach.
constexpr int kDesktopData = 1000;
bool g_logged_table = false;
int g_shown_data = -2;  // the dropdown item (its data) last filled or applied
int g_last_fixed = -2;  // last position written (for the log)
bool g_ready = false;

struct GameMode {
  int w, h, bpp, hz;
};

// The game's display mode list, or 0 entries while it doesn't exist yet.
int Modes(const uint8_t** arr) {
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  if (!game) return 0;
  const uintptr_t dev = *reinterpret_cast<const uintptr_t*>(game + 0xC);
  if (!dev) return 0;
  const int n = *reinterpret_cast<const int*>(dev + 0x8);
  const uint8_t* a = *reinterpret_cast<const uint8_t* const*>(dev + 0x10);
  if (n <= 0 || n > 512 || !a) return 0;
  *arr = a;
  return n;
}

GameMode ModeAt(const uint8_t* arr, int i) {
  const int* m = reinterpret_cast<const int*>(arr + i * 0x24);
  return {m[0], m[1], m[2], m[3]};
}

// The ini's choice for the current Fullscreen mode, checked against what the
// monitor / list offers; the desktop otherwise.
void ChooseFromIni() {
  const Config& c = GetConfig();
  const display::Mode desk = display::Desktop();
  if (c.exclusive) {
    int w = c.fs_width, h = c.fs_height, hz = c.fs_hz;
    const char* why = "ExclusiveResolution";
    if (!w) {
      w = desk.w;
      h = desk.h;
      hz = desk.hz;
      why = "Desktop";
    } else if (!display::MonitorHas(w, h, 0)) {
      patch::Log("resolution: ExclusiveResolution=%dx%d@%d: the monitor has no such mode; desktop used this time", w, h,
                 hz);
      w = desk.w;
      h = desk.h;
      hz = desk.hz;
      why = "Desktop (fallback)";
    } else if (!hz || !display::MonitorHas(w, h, hz)) {
      const int best = display::BestHz(w, h);
      if (hz) patch::Log("resolution: %dx%d has no %d Hz mode; %d Hz used", w, h, hz, best);
      hz = best;
    }
    g_want_w = w;
    g_want_h = h;
    g_want_hz = hz;
    g_want_desktop = why[0] == 'D';
    patch::Log("resolution: Fullscreen=Exclusive, mode %dx%d@%d (%s)", w, h, hz, why);
  } else {
    int w = c.bl_width, h = c.bl_height;
    const char* why = "BorderlessResolution";
    bool listed = false;
    for (const display::Mode& m : display::GameModes()) listed |= m.w == w && m.h == h;
    if (!w) {
      w = desk.w;
      h = desk.h;
      why = "Desktop";
    } else if (!listed) {
      patch::Log("resolution: BorderlessResolution=%dx%d is not available (desktop %dx%d); desktop used this time", w,
                 h, desk.w, desk.h);
      w = desk.w;
      h = desk.h;
      why = "Desktop (fallback)";
    }
    g_want_w = w;
    g_want_h = h;
    g_want_hz = 0;
    g_want_desktop = why[0] == 'D';
    patch::Log("resolution: Fullscreen=Borderless, rendering at %dx%d (%s), scaled to %dx%d", w, h, why, desk.w, desk.h);
  }
}

// Options OK: the player picked the mode at this position (or "Desktop").
void __stdcall NotePick(int pos) {
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  const bool excl = GetConfig().exclusive;
  if (pos == kDesktopData) {
    const display::Mode d = display::Desktop();
    int real = -1;
    for (int i = 0; i < n && real < 0; ++i) {
      const GameMode m = ModeAt(arr, i);
      if (m.w == d.w && m.h == d.h && (!excl || m.hz == d.hz)) real = i;
    }
    for (int i = 0; i < n && real < 0; ++i) {
      const GameMode m = ModeAt(arr, i);
      if (m.w == d.w && m.h == d.h) real = i;
    }
    // The game keeps a mode position: give it the desktop's.
    *reinterpret_cast<int*>(0x68E918) = real >= 0 ? real : 0;
    const char* key = excl ? "ExclusiveResolution" : "BorderlessResolution";
    if (!g_want_desktop) {
      const char* file = SetStringFromMenu(key, "Desktop");
      patch::Log("resolution: player picked Desktop (%dx%d@%d, position %d); saved as %s=Desktop in %s", d.w, d.h,
                 excl ? d.hz : 0, real, key, file);
    }
    g_want_w = d.w;
    g_want_h = d.h;
    g_want_hz = excl ? d.hz : 0;
    g_want_desktop = true;
    return;
  }
  if (pos < 0 || pos >= n) {
    patch::Log("resolution: Options picked position %d, outside the %d modes; not saved", pos, n);
    return;
  }
  const GameMode m = ModeAt(arr, pos);
  const int hz = excl ? m.hz : 0;
  if (m.w == g_want_w && m.h == g_want_h && hz == g_want_hz && !g_want_desktop) return;
  g_want_desktop = false;
  g_want_w = m.w;
  g_want_h = m.h;
  g_want_hz = hz;
  char v[32];
  if (excl)
    std::snprintf(v, sizeof(v), "%dx%d@%d", m.w, m.h, m.hz);
  else
    std::snprintf(v, sizeof(v), "%dx%d", m.w, m.h);
  const char* key = excl ? "ExclusiveResolution" : "BorderlessResolution";
  const char* file = SetStringFromMenu(key, v);
  patch::Log("resolution: player picked %s (position %d); saved as %s=%s in %s%s", v, pos, key, v, file,
             excl ? "; the screen switches now (Direct3D 11) or at the next start (Direct3D 9)" : "");
}

// The dropdown's text for a mode: the game's "W X H", plus the refresh rate
// with Fullscreen=Exclusive. Called instead of the game's sprintf by a stub
// that pushes copies of the arguments (never the game's own stack slots: a
// cdecl callee may reuse its argument slots, and res-1 crashed when a tail
// call overwrote the game's return address that way).
int __cdecl LabelFormat(int pos, char* buf, int w, int h) {
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  int hz = 0;
  if (GetConfig().exclusive && pos >= 0 && pos < n) hz = ModeAt(arr, pos).hz;
  // The game's buffer is on its stack, 0x70 bytes; these stay far below that.
  if (hz > 0) return std::snprintf(buf, 48, "%d X %d  %d Hz", w, h, hz);
  return std::snprintf(buf, 48, "%d X %d", w, h);
}

// Replace "mov [0x68e918],eax" at site with a call to a stub that does the
// same and then reports the position to NotePick.
bool HookPickSite(uintptr_t site) {
  const uint8_t code[] = {0xA3, 0x18, 0xE9, 0x68, 0x00};  // mov [0068e918],eax
  if (!patch::Matches(site, code, sizeof(code))) {
    patch::Log("resolution: Options save code at %08x differs, not hooked", static_cast<unsigned>(site));
    return false;
  }
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return false;
  // mov [0x68e918],eax; pushad; push eax; call NotePick; popad; ret
  size_t n = 0;
  std::memcpy(t + n, code, sizeof(code));
  n += sizeof(code);
  t[n++] = 0x60;
  t[n++] = 0x50;
  t[n++] = 0xE8;
  const int32_t r = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&NotePick) -
                                         reinterpret_cast<uintptr_t>(t + n + 4));
  std::memcpy(t + n, &r, 4);
  n += 4;
  t[n++] = 0x61;
  t[n++] = 0xC3;
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Call(site, t, sizeof(code));
  return true;
}

// Replace "call sprintf(buf, fmt, w, h)" at site (dropdown label) with a
// stub: push h; push w; push buf; push esi; call LabelFormat; add esp,16; ret.
uint8_t* g_label_stub = nullptr;
bool HookLabelSite(uintptr_t site) {
  const uint8_t code[] = {0xE8};
  if (!patch::Matches(site, code, 1) ||
      site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != 0x53A11F) {
    patch::Log("resolution: dropdown label code at %08x differs, not hooked", static_cast<unsigned>(site));
    return false;
  }
  if (!g_label_stub) {
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    // On entry: [esp] ret, +4 buf, +8 fmt, +12 w, +16 h.
    const uint8_t head[] = {
        0xFF, 0x74, 0x24, 0x10,  // push [esp+16]  h
        0xFF, 0x74, 0x24, 0x10,  // push [esp+16]  w
        0xFF, 0x74, 0x24, 0x0C,  // push [esp+12]  buf
        0x56,                    // push esi       mode position
        0xE8};                   // call LabelFormat
    size_t n = sizeof(head);
    std::memcpy(t, head, n);
    const int32_t r = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LabelFormat) -
                                           reinterpret_cast<uintptr_t>(t + n + 4));
    std::memcpy(t + n, &r, 4);
    n += 4;
    t[n++] = 0x83;  // add esp,16
    t[n++] = 0xC4;
    t[n++] = 0x10;
    t[n++] = 0xC3;  // ret
    FlushInstructionCache(GetCurrentProcess(), t, n);
    g_label_stub = t;
  }
  patch::Call(site, g_label_stub, 5);
  return true;
}

// ---- The game's video card test (CardTest=0: replaced) -----------------
// At start the game loads each card's cached test results from the registry
// (DriverInfo) or tests the card (0x4c5460: a 640x480 mode switch, video
// memory, AGP, texture formats, a partial blit), after writing the card's
// identity to the registry (0x4c5e00). With MCM2MaX's renderer the answers
// never change, so both are skipped and the card gets the values the test
// itself gave on Direct3D 11 (res-1 log, 2026-11-16).
constexpr uintptr_t kCards = 0x690894, kCardCount = 0x6908A4;

int __fastcall CardSetup(uintptr_t /*game*/) {
  const int n = *reinterpret_cast<const int*>(kCardCount);
  for (int i = 0; i < n && i < 8; ++i) {
    const uintptr_t card = reinterpret_cast<const uintptr_t*>(kCards)[i];
    if (!card) continue;
    *reinterpret_cast<int*>(card + 0x54) = 256 << 20;      // TotalVideoMemory (GetAvailableVidMem)
    *reinterpret_cast<int*>(card + 0x60) = 0x7FFFFFFF;     // TextureCacheLimit: none
    *reinterpret_cast<int*>(card + 0x5BC) = 0;             // PartialTextureBlt
    *reinterpret_cast<int*>(card + 0x9F0) = 1;             // IsAGP
    const int modes = *reinterpret_cast<const int*>(card + 0x8);
    uint8_t* arr = *reinterpret_cast<uint8_t**>(card + 0x10);
    for (int m = 0; arr && m < modes && m < 512; ++m) {  // every mode usable
      reinterpret_cast<int*>(arr + m * 0x24)[5] = 1;
      reinterpret_cast<int*>(arr + m * 0x24)[6] = 1;
    }
    patch::Log("resolution: card %d \"%s\": the game's card test skipped (CardTest=0); %d modes, video memory 256 MB, "
               "flags70 %02x, disabled %02x",
               i, reinterpret_cast<const char*>(card + 0x4BC), modes, *reinterpret_cast<const uint8_t*>(card + 0x70),
               *reinterpret_cast<const uint8_t*>(card + 0xB74));
  }
  return 1;
}

int __fastcall SkipCardIdentity(uintptr_t) { return 1; }

void ReplaceCardTest() {
  const uint8_t id_call[] = {0xE8};
  const bool a = patch::Matches(0x4C4716, id_call, 1) &&
                 0x4C4716 + 5 + *reinterpret_cast<const int32_t*>(0x4C4717) == 0x4C5E00;
  const bool b = patch::Matches(0x4C471D, id_call, 1) &&
                 0x4C471D + 5 + *reinterpret_cast<const int32_t*>(0x4C471E) == 0x4C5460;
  if (!a || !b) {
    patch::Log("resolution: card test code differs (%d/%d); the game's own test runs", a, b);
    return;
  }
  patch::Call(0x4C4716, reinterpret_cast<void*>(&SkipCardIdentity), 5);
  patch::Call(0x4C471D, reinterpret_cast<void*>(&CardSetup), 5);
  patch::Log("resolution: the game's video card test is replaced (CardTest=1 runs it)");
}

// ---- Borderless / Exclusive switched in Options --------------------------
// The game's mode table is refilled through its own EnumDisplayModes callback
// (0x4cddb0, stdcall(desc, card)), which grows the array as needed.
constexpr uintptr_t kEnumCallback = 0x4CDDB0;
using EnumCbFn = HRESULT(__stdcall*)(void* desc, void* ctx);

void RefillGameTable() {
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  if (!game) return;
  const uintptr_t card = *reinterpret_cast<const uintptr_t*>(game + 0xC);
  if (!card) return;
  const uint8_t cb_start[] = {0x56, 0x8B, 0x74, 0x24, 0x0C};  // push esi; mov esi,[esp+0c]
  if (!patch::Matches(kEnumCallback, cb_start, sizeof(cb_start))) {
    patch::Log("resolution: mode list code differs; the list changes at the next start");
    return;
  }
  const int before = *reinterpret_cast<const int*>(card + 0x8);
  *reinterpret_cast<int*>(card + 0x8) = 0;
  for (const display::Mode& m : display::GameModes()) {
    uint8_t desc[0x7C] = {};
    *reinterpret_cast<uint32_t*>(desc + 0x00) = 0x7C;
    *reinterpret_cast<uint32_t*>(desc + 0x08) = m.h;
    *reinterpret_cast<uint32_t*>(desc + 0x0C) = m.w;
    *reinterpret_cast<uint32_t*>(desc + 0x18) = m.hz ? m.hz : 60;
    *reinterpret_cast<uint32_t*>(desc + 0x54) = 16;
    reinterpret_cast<EnumCbFn>(kEnumCallback)(desc, reinterpret_cast<void*>(card));
  }
  g_logged_table = false;  // log the new table next frame
  g_last_fixed = -2;
  patch::Log("resolution: the game's mode list refilled: %d -> %d modes", before,
             *reinterpret_cast<const int*>(card + 0x8));
}

// The resolution dropdown (list object at control +0x1fc): items 0x38 bytes
// at +0x214, count +0x1ec, selection +0x1f4; text (item +0x14) allocated
// with the game's allocator 0x4a4410, freed with 0x4a4450.
using AddItemFn = int(__fastcall*)(uintptr_t list, void*, const char* text, int data, int flags);
using SelectFn = int(__fastcall*)(uintptr_t list, void*, int data);
using FreeFn = void(__cdecl*)(void* p, const char* file, int line);

void RefillDropdown(uintptr_t control) {
  if (!control || IsBadReadPtr(reinterpret_cast<void*>(control), 0x200)) return;
  const uintptr_t list = *reinterpret_cast<const uintptr_t*>(control + 0x1FC);
  if (!list || IsBadReadPtr(reinterpret_cast<void*>(list), 0x218)) {
    patch::Log("resolution: dropdown list not found");
    return;
  }
  const int count = *reinterpret_cast<const int*>(list + 0x1EC);
  uint8_t* items = *reinterpret_cast<uint8_t**>(list + 0x214);
  for (int i = 0; items && i < count; ++i) {
    void*& text = *reinterpret_cast<void**>(items + i * 0x38 + 0x14);
    if (text) reinterpret_cast<FreeFn>(0x4A4450)(text, "MCM2MaX", 0);
    text = nullptr;
  }
  *reinterpret_cast<int*>(list + 0x1EC) = 0;
  *reinterpret_cast<int*>(list + 0x1F4) = -1;
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  // "Desktop" first: picking it writes Desktop to the ini (NotePick).
  reinterpret_cast<AddItemFn>(0x477D10)(list, nullptr, "Desktop", kDesktopData, 0);
  int select = -1;
  for (int i = 0; i < n; ++i) {
    const GameMode m = ModeAt(arr, i);
    char label[48];
    LabelFormat(i, label, m.w, m.h);
    reinterpret_cast<AddItemFn>(0x477D10)(list, nullptr, label, i, 0);
    if (select < 0 && m.w == g_want_w && m.h == g_want_h && (!g_want_hz || m.hz == g_want_hz)) select = i;
  }
  if (select < 0) select = 0;
  // The page's working copy of the mode position stays a real position.
  *reinterpret_cast<int*>(0x68E918) = select;
  reinterpret_cast<SelectFn>(0x477AC0)(list, nullptr, g_want_desktop ? kDesktopData : select);
  g_shown_data = g_want_desktop ? kDesktopData : select;
  patch::Log("resolution: dropdown filled: %d -> %d entries + Desktop, showing %s (position %d)", count, n,
             g_want_desktop ? "Desktop" : "the mode", select);
}

// ---- "Keep this display mode?" (Fullscreen=Exclusive) ----------------------
// A mode picked in the dropdown is tried at once; unless the player keeps it
// (Enter, or the Keep button) within 10 seconds, the previous mode comes
// back (Esc, the Revert button, or doing nothing). Answers come from
// ModePromptAnswer (keys / clicks seen by input_timing.cpp, or polled here).
constexpr DWORD kPromptMs = 11500;  // 10 s shown, plus the ~1.5 s the switch itself takes
struct Prompt {
  bool on = false;
  DWORD start = 0;
  int prev_data = -2;      // the dropdown item to go back to
  int select_data = -2;    // revert: put the dropdown back on this item (next frame with the page open)
  RECT keep{}, revert{};   // the buttons, in surface pixels (drawn by racetitle.cpp)
  UINT surf_w = 0, surf_h = 0;
  bool keys_down = true;   // Enter/Esc/button held when the prompt opened: wait for release
} g_prompt;
CRITICAL_SECTION* PromptLock() {
  static CRITICAL_SECTION cs;
  static bool init = [] {
    InitializeCriticalSection(&cs);
    return true;
  }();
  (void)init;
  return &cs;
}

// The menu mouse cursor (GUICursor, vtable 0x558f88; update 0x486100 =
// GameCursor::Update 0x43fce0 puts the position at +0x54/+0x58).
using CursorUpdateFn = void(__fastcall*)(uintptr_t self, void* edx);
constexpr uintptr_t kCursorVtableSlot = 0x558F88 + 15 * 4;
constexpr uintptr_t kCursorUpdate = 0x486100;
volatile LONG g_cursor_x = -1, g_cursor_y = -1;
void __fastcall CursorUpdateHook(uintptr_t self, void* edx) {
  MenuNavFrame(self);  // keyboard / controller navigation may move it first
  mg::Frame();         // MenuMaX changes when no tick comes
  reinterpret_cast<CursorUpdateFn>(kCursorUpdate)(self, edx);
  g_cursor_x = *reinterpret_cast<const int*>(self + 0x54);
  g_cursor_y = *reinterpret_cast<const int*>(self + 0x58);
}
bool HookCursor() {
  const uintptr_t want = kCursorUpdate;
  if (!patch::Matches(kCursorVtableSlot, &want, 4)) {
    patch::Log("resolution: menu cursor code differs; the mode prompt's buttons do not take clicks");
    return false;
  }
  const uintptr_t fn = reinterpret_cast<uintptr_t>(&CursorUpdateHook);
  patch::Write(kCursorVtableSlot, &fn, 4);
  return true;
}

void ApplyData(int data);  // below

void PromptEnd(bool keep, const char* how) {
  if (!g_prompt.on) return;
  g_prompt.on = false;
  dd::GpuTextRequestRedraw();  // the prompt was drawn over the page's text
  if (keep) {
    patch::Log("resolution: mode kept (%s)", how);
    return;
  }
  patch::Log("resolution: mode reverted (%s): back to dropdown item data %d", how, g_prompt.prev_data);
  g_prompt.select_data = g_prompt.prev_data;
  ApplyData(g_prompt.prev_data);
}

void PollPromptKeys() {
  // Fallback for the DirectInput filter (input_timing.cpp): the keyboard
  // state Windows keeps. A key must go up once after the prompt opened.
  const bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
  const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  const bool click = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  static bool was_enter = true, was_esc = true, was_click = true;
  if (g_prompt.keys_down) {
    was_enter = enter;
    was_esc = esc;
    was_click = click;
    g_prompt.keys_down = false;
    return;
  }
  if (enter && !was_enter) ModePromptAnswer(kPromptKeep, "Enter (Windows key state)");
  else if (esc && !was_esc) ModePromptAnswer(kPromptRevert, "Esc (Windows key state)");
  else if (click && !was_click) ModePromptAnswer(kPromptClick, "mouse button (Windows key state)");
  was_enter = enter;
  was_esc = esc;
  was_click = click;
}

}  // namespace

void ApplyResolutionDefault() {
  display::Init();
  MigrateDisplayKeys();
  if (!GetConfig().card_test) ReplaceCardTest();
  else patch::Log("resolution: CardTest=1, the game's own video card test runs");
  const bool a = HookPickSite(0x4B6DA3);  // Options > Graphics
  const bool b = HookPickSite(0x453846);
  const bool l1 = HookLabelSite(0x4B6BDB);
  const bool l2 = HookLabelSite(0x4B6C2C);
  const bool cur = HookCursor();
  ChooseFromIni();
  g_ready = true;
  patch::Log("resolution: hooks: Options graphics %d, other %d, labels %d/%d, menu cursor %d", a, b, l1, l2, cur);
}

void ResolutionFillDropdown(uintptr_t dropdown) {
  if (g_ready) RefillDropdown(dropdown);
}

void ResolutionSetExclusive(bool exclusive, uintptr_t dropdown) {
  if (exclusive == GetConfig().exclusive) return;
  const DWORD t0 = GetTickCount();
  const char* file = SetFullscreenFromMenu(exclusive);
  const DWORD t1 = GetTickCount();
  patch::Log("resolution: Fullscreen=%s from Options, saved to %s (%lu ms)", exclusive ? "Exclusive" : "Borderless", file,
             static_cast<unsigned long>(t1 - t0));
  display::Rebuild();
  ChooseFromIni();
  RefillGameTable();
  RefillDropdown(dropdown);
  patch::Log("resolution: switch done in %lu ms (lists %lu ms)", static_cast<unsigned long>(GetTickCount() - t0),
             static_cast<unsigned long>(GetTickCount() - t1));
}

namespace {
void ApplyData(int data) {
  g_shown_data = data;
  if (data != kDesktopData) *reinterpret_cast<int*>(0x68E918) = data;  // the page's working copy
  NotePick(data);
}
}  // namespace

void ResolutionPickNow(uintptr_t control) {
  if (!g_ready || !control || IsBadReadPtr(reinterpret_cast<void*>(control), 0x200)) return;
  const uintptr_t list = *reinterpret_cast<const uintptr_t*>(control + 0x1FC);
  if (!list || IsBadReadPtr(reinterpret_cast<void*>(list), 0x218)) return;
  EnterCriticalSection(PromptLock());
  const int put_back = g_prompt.select_data;
  g_prompt.select_data = -2;
  LeaveCriticalSection(PromptLock());
  if (put_back != -2) {
    reinterpret_cast<SelectFn>(0x477AC0)(list, nullptr, put_back);
    patch::Log("resolution: dropdown back on item data %d", put_back);
    return;
  }
  const int count = *reinterpret_cast<const int*>(list + 0x1EC);
  const int sel = *reinterpret_cast<const int*>(list + 0x1F4);
  const uint8_t* items = *reinterpret_cast<uint8_t* const*>(list + 0x214);
  if (!items || sel < 0 || sel >= count) return;
  const int data = *reinterpret_cast<const int*>(items + sel * 0x38 + 0x1C);
  if (data == g_shown_data) return;
  const int before_w = g_want_w, before_h = g_want_h, before_hz = g_want_hz;
  const int before_data = g_shown_data;
  patch::Log("resolution: dropdown item %d (data %d) picked; applied now", sel, data);
  ApplyData(data);
  const bool changed = g_want_w != before_w || g_want_h != before_h || g_want_hz != before_hz;
  // Direct3D 9 switches at the next start: nothing to confirm now.
  if (!GetConfig().exclusive || !changed || dd::IsD3D9()) return;
  EnterCriticalSection(PromptLock());
  if (!g_prompt.on) g_prompt.prev_data = before_data;
  g_prompt.on = true;
  g_prompt.start = GetTickCount();
  g_prompt.keys_down = true;
  LeaveCriticalSection(PromptLock());
  patch::Log("resolution: asking to keep %dx%d@%d (else back to item data %d in %lu s)", g_want_w, g_want_h,
             g_want_hz, g_prompt.prev_data, static_cast<unsigned long>(kPromptMs / 1000));
}

int ModePromptSeconds() {
  if (!g_prompt.on) return -1;
  const DWORD gone = GetTickCount() - g_prompt.start;
  if (gone >= kPromptMs) return 0;
  const int left = static_cast<int>((kPromptMs - gone + 999) / 1000);
  return left > 10 ? 10 : left;
}

void ModePromptSetButtons(const RECT& keep, const RECT& revert, UINT surf_w, UINT surf_h) {
  EnterCriticalSection(PromptLock());
  g_prompt.keep = keep;
  g_prompt.revert = revert;
  g_prompt.surf_w = surf_w;
  g_prompt.surf_h = surf_h;
  LeaveCriticalSection(PromptLock());
}

bool ModePromptCursor(POINT* p) {
  if (g_cursor_x < 0 || g_cursor_y < 0) return false;
  p->x = g_cursor_x;
  p->y = g_cursor_y;
  return true;
}

void ModePromptAnswer(int answer, const char* how) {
  EnterCriticalSection(PromptLock());
  if (!g_prompt.on || GetTickCount() - g_prompt.start < 300) {
    LeaveCriticalSection(PromptLock());
    return;
  }
  if (answer == kPromptClick) {
    POINT p{};
    const bool have = ModePromptCursor(&p);
    patch::Log("resolution: click during the mode prompt at %ld,%ld (%s; surface %ux%u, keep %ld,%ld-%ld,%ld, revert "
               "%ld,%ld-%ld,%ld)",
               p.x, p.y, have ? "menu cursor" : "no cursor seen", g_prompt.surf_w, g_prompt.surf_h, g_prompt.keep.left,
               g_prompt.keep.top, g_prompt.keep.right, g_prompt.keep.bottom, g_prompt.revert.left, g_prompt.revert.top,
               g_prompt.revert.right, g_prompt.revert.bottom);
    if (!have) answer = 0;
    else if (PtInRect(&g_prompt.keep, p)) answer = kPromptKeep;
    else if (PtInRect(&g_prompt.revert, p)) answer = kPromptRevert;
    else answer = 0;
  }
  if (answer == kPromptKeep || answer == kPromptRevert) PromptEnd(answer == kPromptKeep, how);
  LeaveCriticalSection(PromptLock());
}

bool ModePromptActive() { return g_prompt.on; }

void ResolutionWanted(int* w, int* h, int* hz) {
  if (!g_ready) {
    const display::Mode d = display::Desktop();
    *w = d.w;
    *h = d.h;
    *hz = d.hz;
    return;
  }
  *w = g_want_w;
  *h = g_want_h;
  *hz = g_want_hz;
}

// Called once a frame: point the game's saved position at the wanted mode.
void ResolutionFrame() {
  if (!g_ready) return;
  if (g_prompt.on) {
    PollPromptKeys();
    EnterCriticalSection(PromptLock());
    if (g_prompt.on && GetTickCount() - g_prompt.start >= kPromptMs) PromptEnd(false, "no answer in 10 s");
    LeaveCriticalSection(PromptLock());
  }
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  if (!n) return;
  if (!g_logged_table) {
    g_logged_table = true;
    char line[4096];
    size_t len = 0;
    for (int i = 0; i < n && len + 40 < sizeof(line); ++i) {
      const GameMode m = ModeAt(arr, i);
      const int* r = reinterpret_cast<const int*>(arr + i * 0x24);
      len += std::snprintf(line + len, sizeof(line) - len, " %d:%dx%dx%d@%d/%d%d", i, m.w, m.h, m.bpp, m.hz, r[5],
                           r[6]);
    }
    patch::Log("resolution: the game's %d display modes (pos:WxHxbpp@Hz/usable flags):%s", n, line);
    const uintptr_t dev = *reinterpret_cast<const uintptr_t*>(game + 0xC);
    patch::Log("resolution: card: video memory %d MB, texture cache limit %d, flags70 %02x, partial blt %d, agp %d, "
               "disabled %02x",
               *reinterpret_cast<const int*>(dev + 0x54) >> 20, *reinterpret_cast<const int*>(dev + 0x60),
               *reinterpret_cast<const uint8_t*>(dev + 0x70), *reinterpret_cast<const int*>(dev + 0x5BC),
               *reinterpret_cast<const int*>(dev + 0x9F0), *reinterpret_cast<const uint8_t*>(dev + 0xB74));
  }
  int best = -1;
  for (int i = 0; i < n && best < 0; ++i) {
    const GameMode m = ModeAt(arr, i);
    if (m.w == g_want_w && m.h == g_want_h && (!g_want_hz || m.hz == g_want_hz)) best = i;
  }
  if (best < 0) {
    if (g_last_fixed != -1) {
      patch::Log("resolution: %dx%d@%d is not in the game's list; left at position %d", g_want_w, g_want_h, g_want_hz,
                 *reinterpret_cast<int*>(game + 0xFC4));
      g_last_fixed = -1;
    }
    return;
  }
  int& pos = *reinterpret_cast<int*>(game + 0xFC4);
  if (pos == best) return;
  if (g_last_fixed != best) {
    const GameMode was = pos >= 0 && pos < n ? ModeAt(arr, pos) : GameMode{0, 0, 0, 0};
    patch::Log("resolution: saved position %d (%dx%d@%d) -> %d (%dx%d@%d)", pos, was.w, was.h, was.hz, best,
               g_want_w, g_want_h, g_want_hz);
    g_last_fixed = best;
  }
  pos = best;
}
