// MenuOptions=1: mcm2fix settings on the game's own Options pages.
//
// Each menu page is UI\<NAME>.DAT, an "RS2" archive of the page's sprites
// plus one <Name>.dtm: INI-like text from a Visual Basic form designer that
// lists every control ([Name], Type=, Xywh=, Text=, sprites...). The game
// builds the page from it and finds controls by name.
//
// - Layout: when the game opens UI\OPTGRAPH.DAT we hand it a copy (in
//   %LOCALAPPDATA%\Motocross Madness 2\cache\) whose OptGraph.dtm has our
//   controls added before [Set_Anim]: an "Enhancements" plate on the free right
//   side under Overall Graphics Quality, holding a Motion Blur checkbox. It
//   reuses the page's own plate sprite and the game's shared checkbox sprites.
// - Behaviour: the Options > Graphics page class (vtable 0x55ab2c) gets its
//   event handler (slot 29, 0x4b6ac0) wrapped. On the page's set-up event (5)
//   our checkbox is bound to an int of ours (UIControl::Bind, vtable +0xd8),
//   before the game loads every bound value into its control. After any event
//   the checkbox state (control +0x1f0) is compared with the setting; a change
//   is applied at once and saved to mcm2fix-menu.ini (config.cpp).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "patch.h"
#include "xp_compat.h"
#include "dd/menubg.h"

namespace {

constexpr uintptr_t kIatCreateFileA = 0x5551EC;
constexpr uintptr_t kOptGraphVtable = 0x55AB2C;
constexpr uintptr_t kOptGraphEventSlot = kOptGraphVtable + 29 * 4;
constexpr uintptr_t kOptGraphEvent = 0x4B6AC0;
constexpr uintptr_t kFindControl = 0x46FB20;  // thiscall(dialog, name, type) -> UIControl*
constexpr uintptr_t kUIMultiStateVtable = 0x5582C8;
constexpr size_t kBindSlot = 0xD8;         // UIControl::Bind(int* value)
constexpr size_t kStateOffset = 0x1F0;     // UIMultiState current state

constexpr char kMotionBlurName[] = "Mcm2fixMotionBlur";

// Added to OptGraph.dtm before [Set_Anim]. Positions are page pixels
// (the page itself sits at 48,40 in the 640x480 menu).
constexpr char kOptGraphControls[] =
    "[Mcm2fixBox]\r\n"
    "Type = STATIC\r\n"
    "Height = 1125\r\nLeft = 5010\r\nTabIndex = 20\r\nTop = 2505\r\nWidth = 3420\r\n"
    "_ExtentX = 6033\r\n_ExtentY = 1984\r\n"
    "Show = -1  'True\r\n"
    "TextDrop = -1  'True\r\n"
    "TextAlign = RIGHT\r\n"
    "TextAlignV = MIDDLE\r\n"
    "TextColor = 16777215\r\n"
    "AnimNorm = \"GPlate02\"\r\n"
    "Broken_Enable = 0   'False\r\n"
    "Xywh = 334, 167, 228, 75\r\n"
    "\r\n"
    "[Mcm2fixBoxText]\r\n"
    "Type=STATICTEXT\r\n"
    "Anchor=Mcm2fixBox\r\n"
    "RelAnchor=1\r\n"
    "Height=270\r\nLeft=120\r\nTabIndex=21\r\nTop=90\r\nWidth=3165\r\n"
    "_ExtentX=5583\r\n_ExtentY=476\r\n"
    "TextDrop=-1'True\r\n"
    "TextColor=16777215\r\n"
    "TextAlignV=TOP\r\n"
    "TextAlign=LEFT\r\n"
    "Text=\"Enhancements\"\r\n"
    "Xywh=8,6,211,18\r\n"
    "\r\n"
    "[Mcm2fixMotionBlur]\r\n"
    "NumStates=2\r\n"
    "Type=MULTISTATE\r\n"
    "Anchor=Mcm2fixBox\r\n"
    "RelAnchor=1\r\n"
    "Height=300\r\nLeft=120\r\nTabIndex=22\r\n"
    "ToolTipText=\"Blurs the edges of the picture at speed. Turn off to make the game run faster\"\r\n"
    "Top=480\r\nWidth=3165\r\n"
    "_ExtentX=5583\r\n_ExtentY=529\r\n"
    "DropColor=0\r\n"
    "TextDrop=-1'True\r\n"
    "TextAlignV=TOP\r\n"
    "TextAlign=RIGHT\r\n"
    "SourceBlit=-1'True\r\n"
    "SoundPush=\"Beep\"\r\n"
    "Broken_Enable=0'False\r\n"
    "State0=\"CheckBoxOff\"\r\n"
    "State1=\"CheckBoxOn\"\r\n"
    "StateDisable0=\"CheckBoxOffGrey\"\r\n"
    "StateDisable1=\"CheckBoxOnGrey\"\r\n"
    "Text=\"Motion Blur\"\r\n"
    "TextColor=16777215\r\n"
    "TextLMargin=3\r\n"
    "Xywh=8,32,211,20\r\n"
    "\r\n";

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_create_file = nullptr;

CRITICAL_SECTION g_cs;

bool EndsWithNoCase(const char* s, const char* tail) {
  const size_t n = std::strlen(s), m = std::strlen(tail);
  return n >= m && _stricmp(s + n - m, tail) == 0 && (n == m || s[n - m - 1] == '\\' || s[n - m - 1] == '/');
}

bool ReadWhole(const char* path, std::vector<char>& out) {
  HANDLE h = g_create_file(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const DWORD size = GetFileSize(h, nullptr);
  bool ok = size != INVALID_FILE_SIZE && size > 8 && size < (64u << 20);
  if (ok) {
    out.resize(size);
    DWORD got = 0;
    ok = ReadFile(h, out.data(), size, &got, nullptr) && got == size;
  }
  CloseHandle(h);
  return ok;
}

uint32_t Get32(const std::vector<char>& d, size_t at) {
  uint32_t v;
  std::memcpy(&v, d.data() + at, 4);
  return v;
}
void Put32(std::vector<char>& d, size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); }

// Where an archive entry's offset/size pair sits in the directory, or 0.
size_t FindEntry(const std::vector<char>& d, const char* wanted) {
  if (d.size() < 8 || std::memcmp(d.data(), "RS2", 4) != 0) return patch::Log("menus: not an RS2 archive"), 0;
  const uint32_t count = Get32(d, 4);
  size_t p = 8;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 4 > d.size()) return patch::Log("menus: archive directory cut short"), 0;
    const uint32_t len = Get32(d, p);
    p += 4;
    if (len == 0 || len > 260 || p + len + 8 > d.size()) return patch::Log("menus: bad archive entry %u", i), 0;
    const std::string name(d.data() + p, strnlen(d.data() + p, len));
    p += len;
    if (_stricmp(name.c_str(), wanted) == 0) {
      const uint32_t off = Get32(d, p), size = Get32(d, p + 4);
      if (off > d.size() || size > d.size() - off) return patch::Log("menus: %s out of range", wanted), 0;
      return p;
    }
    p += 8;
  }
  patch::Log("menus: %s not in the archive", wanted);
  return 0;
}

// The new data goes at the end of the archive; the entry points there.
void ReplaceEntry(std::vector<char>& d, size_t entry, const void* data, size_t n) {
  const size_t at = d.size();
  d.insert(d.end(), static_cast<const char*>(data), static_cast<const char*>(data) + n);
  Put32(d, entry, static_cast<uint32_t>(at));
  Put32(d, entry + 4, static_cast<uint32_t>(n));
}

// OptGraph.dtm with our controls before [Set_Anim].
bool EditOptGraph(std::vector<char>& d) {
  const size_t e = FindEntry(d, "OptGraph.dtm");
  if (!e) return false;
  std::string text(d.data() + Get32(d, e), Get32(d, e + 4));
  if (text.find(kMotionBlurName) != std::string::npos) return patch::Log("menus: OptGraph.dtm already has our controls"), false;
  const size_t anim = text.find("[Set_Anim]");
  if (anim == std::string::npos) return patch::Log("menus: OptGraph.dtm has no [Set_Anim]"), false;
  const size_t before = text.size();
  text.insert(anim, kOptGraphControls);
  ReplaceEntry(d, e, text.data(), text.size());
  patch::Log("menus: Options > Graphics layout: added Enhancements box + Motion Blur checkbox (dtm %u -> %u bytes)",
             static_cast<unsigned>(before), static_cast<unsigned>(text.size()));
  return true;
}

// MenuFlag: the main menu background becomes flat key green (dd/menubg.cpp
// draws the flag and the MenuBackground picture where it shows).
bool EditMainMenu(std::vector<char>& d) {
  if (!dd::MenuBgWanted()) return false;
  const size_t e = FindEntry(d, "01_Main\\BackGround_01.tga");
  if (!e) return false;
  std::vector<char> tga(18 + 640 * 480 * 2);
  tga[2] = 2;  // uncompressed true colour
  tga[12] = static_cast<char>(640 & 0xff);
  tga[13] = static_cast<char>(640 >> 8);
  tga[14] = static_cast<char>(480 & 0xff);
  tga[15] = static_cast<char>(480 >> 8);
  tga[16] = 16;
  for (size_t i = 18; i < tga.size(); i += 2) {
    tga[i] = static_cast<char>(dd::kMenuBgKey555 & 0xff);
    tga[i + 1] = static_cast<char>(dd::kMenuBgKey555 >> 8);
  }
  const uint32_t old_size = Get32(d, e + 4);
  ReplaceEntry(d, e, tga.data(), tga.size());
  patch::Log("menus: main menu background (%u bytes) replaced by key green for the flag", old_size);
  return true;
}

struct PatchedArchive {
  const char* file;  // name the game opens, e.g. OPTGRAPH.DAT
  bool (*edit)(std::vector<char>& d);
  bool on;
  bool tried;
  std::string path;  // the patched copy, empty = the game's own file
};
PatchedArchive g_archives[] = {
    {"OPTGRAPH.DAT", &EditOptGraph, false, false, {}},
    {"MAINMENU.DAT", &EditMainMenu, false, false, {}},
};

// Makes the patched copy of the archive the game asked for. Returns its path,
// or empty (the game then gets its own file).
std::string MakePatched(const char* original, PatchedArchive& a) {
  std::vector<char> d;
  if (!ReadWhole(original, d)) return patch::Log("menus: could not read %s", original), std::string();
  if (!a.edit(d)) return {};
  const std::string root = xp::LocalAppData();
  if (root.empty()) return patch::Log("menus: no per-user folder"), std::string();
  const std::string dir = root + "\\Motocross Madness 2\\cache";
  CreateDirectoryA((root + "\\Motocross Madness 2").c_str(), nullptr);
  CreateDirectoryA(dir.c_str(), nullptr);
  const std::string out = dir + "\\" + a.file;
  HANDLE h = g_create_file(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return patch::Log("menus: cannot write %s (%lu)", out.c_str(), GetLastError()), std::string();
  DWORD put = 0;
  const bool ok = WriteFile(h, d.data(), static_cast<DWORD>(d.size()), &put, nullptr) && put == d.size();
  CloseHandle(h);
  if (!ok) return patch::Log("menus: write of %s failed", out.c_str()), std::string();
  patch::Log("menus: %s -> %s", original, out.c_str());
  return out;
}

void ApplyIfChanged(const char* when, const char* control);

HANDLE WINAPI MenuCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                              DWORD flags, HANDLE templ) {
  // Options closed and a track loading: the saved checkbox value is in by now.
  ApplyIfChanged("file open", name);
  if (name && !(access & GENERIC_WRITE) && disposition == OPEN_EXISTING && std::strstr(name, "\\cache\\") == nullptr) {
    for (PatchedArchive& a : g_archives) {
      if (!a.on || !EndsWithNoCase(name, a.file)) continue;
      EnterCriticalSection(&g_cs);
      if (!a.tried) {
        a.tried = true;
        a.path = MakePatched(name, a);
      }
      const std::string patched = a.path;
      LeaveCriticalSection(&g_cs);
      if (!patched.empty()) {
        HANDLE h = g_create_file(patched.c_str(), access, share, sa, disposition, flags, templ);
        if (h != INVALID_HANDLE_VALUE) {
          if (GetConfig().menu_log) patch::Log("menus: %s -> %s", name, patched.c_str());
          return h;
        }
        patch::Log("menus: could not open %s (%lu), using the game's file", patched.c_str(), GetLastError());
      }
      break;
    }
  }
  return g_create_file(name, access, share, sa, disposition, flags, templ);
}

// ---- Options > Graphics page events ------------------------------------------

using EventFn = void(__fastcall*)(uintptr_t self, void* edx, uintptr_t event);
using FindControlFn = uintptr_t(__fastcall*)(uintptr_t self, void* edx, const char* name, int type);
using BindFn = void(__fastcall*)(uintptr_t self, void* edx, int* value);

int g_motion_blur_value = 0;  // bound to the checkbox while the page is up
unsigned g_events_logged = 0;
bool g_bound = false;  // the checkbox has been bound to g_motion_blur_value

uintptr_t FindOurCheckbox(uintptr_t page) {
  const uintptr_t c = reinterpret_cast<FindControlFn>(kFindControl)(page, nullptr, kMotionBlurName, 0);
  if (!c) return 0;
  if (IsBadReadPtr(reinterpret_cast<void*>(c), kStateOffset + 4) ||
      *reinterpret_cast<uintptr_t*>(c) != kUIMultiStateVtable) {
    patch::Log("menus: %s is not a checkbox (vtable %08x)", kMotionBlurName,
               c ? static_cast<unsigned>(*reinterpret_cast<uintptr_t*>(c)) : 0u);
    return 0;
  }
  return c;
}

// The game writes the checkbox state into g_motion_blur_value when it saves
// the page's values; act on it whenever we get to look.
void ApplyIfChanged(const char* when, const char* control) {
  if (!g_bound) return;
  const bool on = g_motion_blur_value != 0;
  if (on == (GetConfig().motion_blur != 0)) return;
  SetMotionBlurFromMenu(on);
  patch::Log("menus: Motion Blur %s from Options > Graphics (%s%s%s); saved to %s", on ? "on" : "off", when,
             control ? ", " : "", control ? control : "", MenuIniPath());
}

const char* SafeName(uintptr_t event) {
  const char* n = *reinterpret_cast<const char* const*>(event + 4);
  if (!n || IsBadStringPtrA(n, 64)) return "-";
  return n;
}

void __fastcall OptGraphEvent(uintptr_t self, void* edx, uintptr_t event) {
  const int type = *reinterpret_cast<const int*>(event + 8);
  const bool log = GetConfig().menu_log || g_events_logged < 40;
  if (log) {
    ++g_events_logged;
    patch::Log("menus: graphics page event %d control %s", type, SafeName(event));
  }
  if (type == 5) {  // page set-up: bind before the game loads values into controls
    const uintptr_t box = FindOurCheckbox(self);
    if (box) {
      g_motion_blur_value = GetConfig().motion_blur ? 1 : 0;
      const uintptr_t bind = *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(box) + kBindSlot);
      reinterpret_cast<BindFn>(bind)(box, nullptr, &g_motion_blur_value);
      g_bound = true;
      patch::Log("menus: Motion Blur checkbox found, bound (MotionBlur=%d)", g_motion_blur_value);
    } else {
      patch::Log("menus: Motion Blur checkbox not on the page");
    }
  }
  reinterpret_cast<EventFn>(kOptGraphEvent)(self, edx, event);
  if (type == 5) {
    if (const uintptr_t box = FindOurCheckbox(self))
      patch::Log("menus: after set-up the checkbox shows %d",
                 *reinterpret_cast<const int*>(box + kStateOffset));
    return;
  }
  // A click (event 3, the one the game's own handler acts on): read the
  // checkbox itself. Other events only check the bound value (the page may
  // be going away, so no control lookups then).
  if (type == 3) {
    if (const uintptr_t box = FindOurCheckbox(self)) {
      const int state = *reinterpret_cast<const int*>(box + kStateOffset);
      if (log) patch::Log("menus: checkbox state %d", state);
      if ((state != 0) != (GetConfig().motion_blur != 0)) {
        g_motion_blur_value = state != 0;
        ApplyIfChanged("click", SafeName(event));
      }
    }
  }
  ApplyIfChanged("page event", nullptr);
}

}  // namespace

void ApplyMenuOptions() {
  const Config& c = GetConfig();
  g_archives[0].on = c.menu_options && *reinterpret_cast<const uintptr_t*>(kOptGraphEventSlot) == kOptGraphEvent;
  g_archives[1].on = c.menu_flag;
  if (c.menu_options && !g_archives[0].on) patch::Log("menus: Options > Graphics page code differs, not applied");
  if (!c.menu_options) patch::Log("menus: MenuOptions=0, the game's Options pages as they were");
  if (!g_archives[0].on && !g_archives[1].on) return;
  InitializeCriticalSection(&g_cs);
  g_create_file = reinterpret_cast<CreateFileAFn>(patch::HookIat(kIatCreateFileA, reinterpret_cast<void*>(&MenuCreateFileA)));
  if (!g_create_file) g_create_file = &CreateFileA;
  if (g_archives[0].on) {
    void* hook = reinterpret_cast<void*>(&OptGraphEvent);
    patch::Write(kOptGraphEventSlot, &hook, sizeof(hook));
    patch::Log("menus: on: Options > Graphics gets an Enhancements box (Motion Blur); menu choices saved to %s; MenuLog=%d",
               MenuIniPath()[0] ? MenuIniPath() : "(no per-user folder)", c.menu_log ? 1 : 0);
  }
  if (g_archives[1].on) patch::Log("menus: MenuFlag=1, MenuBackground=%s (checked when the main menu loads)", c.menu_background);
}
