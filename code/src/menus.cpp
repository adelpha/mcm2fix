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
//   side under Overall Graphics Quality, holding a checkbox per setting in
//   g_checks. The plate is a frame only (BorderPlate); the checkboxes use the
//   game's shared sprites; labels are set at page set-up (SetLabel).
// - Behaviour: the Options > Graphics page class (vtable 0x55ab2c) gets its
//   event handler (slot 29, 0x4b6ac0) wrapped. On the page's set-up event (5)
//   each checkbox is bound to an int of ours (UIControl::Bind, vtable +0xd8),
//   before the game loads every bound value into its control. After any event
//   the checkbox states (control +0x1f0) are compared with the settings; a
//   change is saved to mcm2fix-menu.ini (config.cpp) and, for live settings,
//   applied at once.
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

// The Enhancements box (Options > Graphics): mcm2fix switches as checkboxes.
// Live ones change at once; the others are saved and apply at the next start
// (their hooks are put in when the game starts), marked with a '*'.
struct MenuCheck {
  const char* name;   // dtm control name
  const char* label;
  const char* key;    // ini key
  int def_on;         // value written for "on" when the setting is off now
  bool live;
  int value;          // bound to the checkbox while the page is up (0/1)
  int on_value;       // value written for "on"
  int saved;          // value as last saved (or loaded)
};
MenuCheck g_checks[] = {
    {"Mcm2fixMotionBlur", "Motion Blur", "MotionBlur", 1, true},
    {"Mcm2fixSmoothEdges", "Smooth Tree Edges", "AlphaToCoverage", 2, true},
    {"Mcm2fixVSync", "VSync", "VSync", 1, true},
    {"Mcm2fixShadows", "Soft Rider Shadows *", "RiderShadows", 2, false},
    {"Mcm2fixReflections", "Bike Reflections *", "Reflections", 2, false},
    {"Mcm2fixWideView", "Widescreen View *", "WideFov", 1, false},
};
constexpr int kNumChecks = static_cast<int>(sizeof(g_checks) / sizeof(g_checks[0]));
constexpr char kFootName[] = "Mcm2fixFootnote";
constexpr char kFootText[] = "* after restarting the game";
// Page layout (control pixels; the page's controls are placed 20,35 menu
// pixels further on). StOo: every box a border-only plate; the two columns
// moved up and left to 20 screen pixels (at 1080p, 9 menu pixels) from the
// bezel's innermost point and below the tab line, 9 apart; uniform checkbox
// rows, less space under the titles.
constexpr int kGap = 9;
constexpr int kBoxW = 228;
constexpr int kLeftX = 8, kRightX = kLeftX + kBoxW + kGap;
constexpr int kTopY = 7, kTopH = 66;
constexpr int kLowY = kTopY + kTopH + kGap;
constexpr int kRowTop = 27, kRowStep = 22;  // first checkbox, checkbox to checkbox
constexpr int kFeaturesH = kRowTop + 2 * kRowStep + 20 + 7;
constexpr int kBoxH = kRowTop + kNumChecks * kRowStep + 22;

int CurrentValue(const char* key) {
  const Config& c = GetConfig();
  if (!std::strcmp(key, "MotionBlur")) return c.motion_blur ? 1 : 0;
  if (!std::strcmp(key, "AlphaToCoverage")) return c.alpha_to_coverage;
  if (!std::strcmp(key, "VSync")) return c.vsync ? 1 : 0;
  if (!std::strcmp(key, "RiderShadows")) return c.rider_shadows;
  if (!std::strcmp(key, "Reflections")) return c.reflections;
  if (!std::strcmp(key, "WideFov")) return c.wide_fov ? 1 : 0;
  return 0;
}

// Added to OptGraph.dtm before [Set_Anim]. Positions are page pixels
// (the page itself sits at 48,40 in the 640x480 menu); the box goes on the
// free right side under Overall Graphics Quality, level with Graphic Features.
std::string OptGraphControls() {
  std::string t;
  char buf[2048];
  std::snprintf(buf, sizeof(buf),
                "[Mcm2fixBox]\r\nType = STATIC\r\nHeight = %d\r\nLeft = 5010\r\nTabIndex = 20\r\nTop = 2505\r\n"
                "Width = 3420\r\nShow = -1  'True\r\nTextDrop = -1  'True\r\nTextAlign = RIGHT\r\n"
                "TextAlignV = MIDDLE\r\nTextColor = 16777215\r\nAnimNorm = \"Mcm2fixPlate\"\r\n"
                "Broken_Enable = 0   'False\r\nXywh = %d, %d, 228, %d\r\n\r\n"
                "[Mcm2fixBoxText]\r\nType=STATICTEXT\r\nAnchor=Mcm2fixBox\r\nRelAnchor=1\r\nHeight=270\r\nLeft=120\r\n"
                "TabIndex=21\r\nTop=90\r\nWidth=3165\r\nTextDrop=-1'True\r\nTextColor=16777215\r\nTextAlignV=TOP\r\n"
                "TextAlign=LEFT\r\nText=\"Enhancements\"\r\nXywh=8,6,211,18\r\n\r\n",
                kBoxH * 15, kRightX, kLowY, kBoxH);
  t += buf;
  for (int i = 0; i < kNumChecks; ++i) {
    const int y = kRowTop + i * kRowStep;
    std::snprintf(buf, sizeof(buf),
                  "[%s]\r\nNumStates=2\r\nType=MULTISTATE\r\nAnchor=Mcm2fixBox\r\nRelAnchor=1\r\nHeight=300\r\n"
                  "Left=120\r\nTabIndex=%d\r\nTop=%d\r\nWidth=3165\r\nDropColor=0\r\nTextDrop=-1'True\r\n"
                  "TextAlignV=TOP\r\nTextAlign=RIGHT\r\nSourceBlit=-1'True\r\nSoundPush=\"Beep\"\r\n"
                  "Broken_Enable=0'False\r\nState0=\"CheckBoxOff\"\r\nState1=\"CheckBoxOn\"\r\n"
                  "StateDisable0=\"CheckBoxOffGrey\"\r\nStateDisable1=\"CheckBoxOnGrey\"\r\nText=\"%s\"\r\n"
                  "TextColor=16777215\r\nTextLMargin=3\r\nXywh=8,%d,211,20\r\n\r\n",
                  g_checks[i].name, 22 + i, y * 15, g_checks[i].label, y);
    t += buf;
  }
  std::snprintf(buf, sizeof(buf),
                "[%s]\r\nType=STATICTEXT\r\nAnchor=Mcm2fixBox\r\nRelAnchor=1\r\nHeight=240\r\nLeft=120\r\n"
                "TabIndex=%d\r\nTop=%d\r\nWidth=3165\r\nTextDrop=-1'True\r\nTextColor=16777215\r\nTextAlignV=TOP\r\n"
                "TextAlign=LEFT\r\nText=\"%s\"\r\nXywh=8,%d,211,16\r\n\r\n",
                kFootName, 22 + kNumChecks, (kRowTop + kNumChecks * kRowStep) * 15, kFootText,
                kRowTop + kNumChecks * kRowStep);
  t += buf;
  return t;
}

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

// Adds a file to the archive (rebuilds the directory; the data stays where it is,
// only shifted by the longer directory).
void AddEntry(std::vector<char>& d, const char* name, const std::vector<char>& data) {
  struct E { std::string name; uint32_t off, size; };
  std::vector<E> es;
  const uint32_t count = Get32(d, 4);
  size_t p = 8;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t len = Get32(d, p);
    p += 4;
    E e{std::string(d.data() + p, strnlen(d.data() + p, len)), 0, 0};
    p += len;
    e.off = Get32(d, p);
    e.size = Get32(d, p + 4);
    p += 8;
    es.push_back(e);
  }
  const size_t old_dir_end = p;
  std::vector<char> out(d.begin(), d.begin() + 4);
  out.resize(8);
  Put32(out, 4, count + 1);
  size_t dir = 8;
  for (const E& e : es) dir += 4 + e.name.size() + 1 + 8;
  dir += 4 + std::strlen(name) + 1 + 8;
  const size_t shift = dir - old_dir_end;
  auto put_entry = [&](const std::string& n, uint32_t off, uint32_t size) {
    const size_t at = out.size();
    out.resize(at + 4 + n.size() + 1 + 8);
    Put32(out, at, static_cast<uint32_t>(n.size() + 1));
    std::memcpy(out.data() + at + 4, n.c_str(), n.size() + 1);
    Put32(out, at + 4 + n.size() + 1, off);
    Put32(out, at + 4 + n.size() + 1 + 4, size);
  };
  for (const E& e : es) put_entry(e.name, static_cast<uint32_t>(e.off + shift), e.size);
  put_entry(name, static_cast<uint32_t>(d.size() + shift), static_cast<uint32_t>(data.size()));
  out.insert(out.end(), d.begin() + old_dir_end, d.end());
  out.insert(out.end(), data.begin(), data.end());
  d.swap(out);
}

// A plate that is only a frame: the 2-pixel edge of one of the page's own
// plates (stretched to w x h), magenta inside. Magenta is the game's sprite
// transparency key, so the page background shows through and the panel
// matches wherever it is put (the game's plates have the background baked
// in, lit for the place they were cut from).
bool BorderPlate(const std::vector<char>& d, const char* src_name, int w, int h, std::vector<char>& tga) {
  const size_t e = FindEntry(d, src_name);
  if (!e) return false;
  const uint32_t off = Get32(d, e), size = Get32(d, e + 4);
  const unsigned char* s = reinterpret_cast<const unsigned char*>(d.data() + off);
  if (size < 18 || s[2] != 2 || s[16] != 16) return patch::Log("menus: %s is not a plain 16-bit TGA", src_name), false;
  const int sw = s[12] | s[13] << 8, sh = s[14] | s[15] << 8;
  const size_t start = 18 + s[0];
  if (sw < 8 || sh < 8 || start + static_cast<size_t>(sw) * sh * 2 > size) return patch::Log("menus: %s too small", src_name), false;
  auto map = [](int v, int n, int sn) { return v < 2 ? v : v >= n - 2 ? sn - (n - v) : 2 + (v - 2) * (sn - 4) / (n - 4); };
  tga.assign(18 + static_cast<size_t>(w) * h * 2, 0);
  std::memcpy(tga.data(), s, 18);
  tga[0] = 0;
  tga[12] = static_cast<char>(w & 0xff);
  tga[13] = static_cast<char>(w >> 8);
  tga[14] = static_cast<char>(h & 0xff);
  tga[15] = static_cast<char>(h >> 8);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint16_t v = 0x7C1F;  // magenta (X1R5G5B5): see-through
      if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) {
        const size_t si = start + (static_cast<size_t>(map(y, h, sh)) * sw + map(x, w, sw)) * 2;
        v = static_cast<uint16_t>(s[si] | s[si + 1] << 8);
      }
      std::memcpy(tga.data() + 18 + (static_cast<size_t>(y) * w + x) * 2, &v, 2);
    }
  return true;
}

// Sets Key in [section] (the first such line in it), or adds it.
bool SetKey(std::string& t, const char* section, const char* key, const std::string& value) {
  const std::string head = std::string("[") + section + "]";
  const size_t sec = t.find(head);
  if (sec == std::string::npos) return patch::Log("menus: no [%s] in the layout", section), false;
  size_t end = t.find("\n[", sec + head.size());
  if (end == std::string::npos) end = t.size();
  const size_t klen = std::strlen(key);
  for (size_t line = t.find('\n', sec); line != std::string::npos && line < end; line = t.find('\n', line + 1)) {
    const size_t a = line + 1;
    if (t.compare(a, klen, key) != 0) continue;
    size_t q = a + klen;
    while (q < t.size() && t[q] == ' ') ++q;
    if (q >= t.size() || t[q] != '=') continue;
    size_t eol = t.find('\n', a);
    if (eol == std::string::npos) eol = t.size();
    if (eol > a && t[eol - 1] == '\r') --eol;
    t.replace(a, eol - a, std::string(key) + "=" + value);
    return true;
  }
  const size_t eol = t.find('\n', sec);
  t.insert(eol + 1, std::string(key) + "=" + value + "\r\n");
  return true;
}

// Moves/resizes a control: Xywh in pixels and the twips the designer also wrote.
void Place(std::string& t, const char* section, int x, int y, int w, int h) {
  char v[64];
  std::snprintf(v, sizeof(v), "%d,%d,%d,%d", x, y, w, h);
  SetKey(t, section, "Xywh", v);
  SetKey(t, section, "Left", std::to_string(x * 15));
  SetKey(t, section, "Top", std::to_string(y * 15));
  SetKey(t, section, "Width", std::to_string(w * 15));
  SetKey(t, section, "Height", std::to_string(h * 15));
}

struct Plate {
  const char* anim;  // sprite section and file name
  int h;
};
constexpr Plate kPlates[] = {{"Mcm2fixPlateTop", kTopH}, {"Mcm2fixPlateFeatures", kFeaturesH}, {"Mcm2fixPlate", kBoxH}};

// The game's own boxes: border-only plates, new places, tighter rows.
void RelayoutOptGraph(std::string& t) {
  Place(t, "DisplayResolutionBox", kLeftX, kTopY, kBoxW, kTopH);
  SetKey(t, "DisplayResolutionBox", "AnimNorm", "\"Mcm2fixPlateTop\"");
  Place(t, "DisplayResolutionDropDown", 25, 28, 178, 83);
  Place(t, "TerrainQualityBox", kRightX, kTopY, kBoxW, kTopH);
  SetKey(t, "TerrainQualityBox", "AnimNorm", "\"Mcm2fixPlateTop\"");
  Place(t, "TerrainQualitySliderBar", 11, 28, 204, 16);
  Place(t, "rbwLabel6", 11, 45, 15, 17);
  Place(t, "rbwLabel7", 197, 45, 18, 17);
  Place(t, "GraphicFeaturesBox", kLeftX, kLowY, kBoxW, kFeaturesH);
  SetKey(t, "GraphicFeaturesBox", "AnimNorm", "\"Mcm2fixPlateFeatures\"");
  Place(t, "ShadowsCheckBox", 8, kRowTop, 211, 20);
  Place(t, "ParticlesCheckBox", 8, kRowTop + kRowStep, 211, 20);
  Place(t, "SkyCheckBox", 8, kRowTop + 2 * kRowStep, 211, 20);
}

// OptGraph.dtm with our controls before [Set_Anim].
bool EditOptGraph(std::vector<char>& d) {
  const size_t e = FindEntry(d, "OptGraph.dtm");
  if (!e) return false;
  std::string text(d.data() + Get32(d, e), Get32(d, e + 4));
  if (text.find(g_checks[0].name) != std::string::npos) return patch::Log("menus: OptGraph.dtm already has our controls"), false;
  const size_t anim = text.find("[Set_Anim]");
  if (anim == std::string::npos) return patch::Log("menus: OptGraph.dtm has no [Set_Anim]"), false;
  const size_t before = text.size();
  std::vector<char> plates[3];
  std::string anims;
  for (int i = 0; i < 3; ++i) {
    if (!BorderPlate(d, "Options\\Graphics\\GPlate03.tga", kBoxW, kPlates[i].h, plates[i])) return false;
    anims += std::string("[") + kPlates[i].anim + "]\r\nFiles1 = Options\\Graphics\\" + kPlates[i].anim +
             ".tga\r\nMSecDelay = 0\r\nLoopCount = 0\r\n\r\n";
  }
  text.insert(anim + std::strlen("[Set_Anim]\r\n"), anims);
  text.insert(anim, OptGraphControls());
  RelayoutOptGraph(text);
  ReplaceEntry(d, e, text.data(), text.size());
  for (int i = 0; i < 3; ++i)
    AddEntry(d, (std::string("Options\\Graphics\\") + kPlates[i].anim + ".tga").c_str(), plates[i]);
  patch::Log("menus: border-only plates added (228x%d, 228x%d, 228x%d; edge of GPlate03, magenta inside)", kTopH,
             kFeaturesH, kBoxH);
  patch::Log("menus: Options > Graphics layout: added Enhancements box with %d checkboxes (dtm %u -> %u bytes)", kNumChecks,
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
using SetTextFn = void(__fastcall*)(uintptr_t self, void* edx, const char* text);
using RedrawFn = void(__fastcall*)(uintptr_t self, void* edx);

// The game only shows a control's text when the layout gives it a
// LocalTextId (a string in UILANG.DLL); our controls have none, so their
// Text= is set here the way the game sets a LocalTextId string
// (0x4719b0: UIControl::SetText 0x471a50, then vtable +0xc8 to redraw).
constexpr uintptr_t kSetText = 0x471A50;
constexpr size_t kRedrawSlot = 0xC8;

void SetLabel(uintptr_t page, const char* name, const char* text) {
  const uintptr_t c = reinterpret_cast<FindControlFn>(kFindControl)(page, nullptr, name, 0);
  if (!c || IsBadReadPtr(reinterpret_cast<void*>(c), 4)) {
    patch::Log("menus: %s not on the page, no label", name);
    return;
  }
  reinterpret_cast<SetTextFn>(kSetText)(c, nullptr, text);
  const uintptr_t redraw = *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(c) + kRedrawSlot);
  reinterpret_cast<RedrawFn>(redraw)(c, nullptr);
  patch::Log("menus: %s labelled \"%s\" (control %08x, vtable %08x)", name, text, static_cast<unsigned>(c),
             static_cast<unsigned>(*reinterpret_cast<uintptr_t*>(c)));
}

unsigned g_events_logged = 0;
bool g_bound = false;  // the checkboxes have been bound to g_checks[].value

uintptr_t FindCheckbox(uintptr_t page, const char* name) {
  const uintptr_t c = reinterpret_cast<FindControlFn>(kFindControl)(page, nullptr, name, 0);
  if (!c) return 0;
  if (IsBadReadPtr(reinterpret_cast<void*>(c), kStateOffset + 4) ||
      *reinterpret_cast<uintptr_t*>(c) != kUIMultiStateVtable) {
    patch::Log("menus: %s is not a checkbox (vtable %08x)", name,
               c ? static_cast<unsigned>(*reinterpret_cast<uintptr_t*>(c)) : 0u);
    return 0;
  }
  return c;
}

void InitChecks() {
  static bool done = false;
  if (done) return;
  done = true;
  for (MenuCheck& m : g_checks) {
    const int v = CurrentValue(m.key);
    m.saved = v;
    m.on_value = v ? v : m.def_on;
  }
}

// The game writes each checkbox's state into its bound value when it saves
// the page's values; act on any change whenever we get to look.
void ApplyIfChanged(const char* when, const char* control) {
  if (!g_bound) return;
  for (MenuCheck& m : g_checks) {
    const int want = m.value ? m.on_value : 0;
    if ((want != 0) == (m.saved != 0)) continue;
    m.saved = want;
    SetFromMenu(m.key, want);
    patch::Log("menus: %s=%d from Options > Graphics (%s%s%s)%s; saved to %s", m.key, want, when, control ? ", " : "",
               control ? control : "", m.live ? ", applied now" : ", applies at the next start", MenuIniPath());
  }
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
    InitChecks();
    int bound = 0;
    for (MenuCheck& m : g_checks) {
      const uintptr_t box = FindCheckbox(self, m.name);
      if (!box) {
        patch::Log("menus: %s not on the page", m.name);
        continue;
      }
      m.value = m.saved ? 1 : 0;
      const uintptr_t bind = *reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(box) + kBindSlot);
      reinterpret_cast<BindFn>(bind)(box, nullptr, &m.value);
      ++bound;
    }
    g_bound = bound > 0;
    patch::Log("menus: %d of %d checkboxes bound", bound, kNumChecks);
  }
  reinterpret_cast<EventFn>(kOptGraphEvent)(self, edx, event);
  if (type == 5) {
    SetLabel(self, "Mcm2fixBoxText", "Enhancements");
    for (const MenuCheck& m : g_checks) SetLabel(self, m.name, m.label);
    SetLabel(self, kFootName, kFootText);
    return;
  }
  // A click (event 3, the one the game's own handler acts on): read the
  // clicked checkbox itself. Other events only check the bound values (the
  // page may be going away, so no control lookups then).
  if (type == 3) {
    const char* clicked = SafeName(event);
    for (MenuCheck& m : g_checks) {
      if (std::strcmp(clicked, m.name) != 0) continue;
      if (const uintptr_t box = FindCheckbox(self, m.name)) {
        const int state = *reinterpret_cast<const int*>(box + kStateOffset);
        if (log) patch::Log("menus: %s state %d", m.name, state);
        m.value = state != 0;
      }
    }
    ApplyIfChanged("click", clicked);
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
    patch::Log("menus: on: Options > Graphics gets an Enhancements box (%d settings); menu choices saved to %s; MenuLog=%d",
               kNumChecks, MenuIniPath()[0] ? MenuIniPath() : "(no per-user folder)", c.menu_log ? 1 : 0);
  }
  if (g_archives[1].on) patch::Log("menus: MenuFlag=1, MenuBackground=%s (checked when the main menu loads)", c.menu_background);
}
