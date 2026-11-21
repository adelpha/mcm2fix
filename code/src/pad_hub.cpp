// ControllerHub on SDL3: every connected gamepad feeds one game controller.
//
// SDL runs on the controller sampling thread (xinput_pad.cpp): Init and Pump
// are only ever called there. SDL finds Xbox pads (XInput / RawInput),
// PlayStation and Switch pads (HIDAPI) and other DirectInput gamepads it has a
// mapping for, and hands them over in one layout. Joysticks it can't map
// (wheels, flight sticks) are not opened: the game keeps seeing those as its
// own DirectInput devices.
//
// The pad that was used last drives: a button press, a trigger pulled past a
// quarter or a stick pushed past half way makes a pad the driving one. If the
// driving pad is unplugged, the most recently used remaining pad takes over.

#include "pad_hub.h"

#include <SDL3/SDL.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "patch.h"

namespace hub {
namespace {

struct Pad {
  SDL_JoystickID id = 0;
  SDL_Gamepad* g = nullptr;
  std::string name;
  WORD vid = 0, pid = 0;
  SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
  bool enabled = true;
  PadState st = {};
  Uint64 last_used = 0;  // SDL_GetTicksNS of the last press / push
  WORD rumble_low = 0, rumble_high = 0;
  Uint64 rumble_sent = 0;
  DWORD presses = 0;     // for the summary
};

CRITICAL_SECTION g_cs;
bool g_ready = false, g_log = false;
std::vector<Pad> g_pads;  // open gamepads, in the order SDL reported them
std::vector<std::pair<WORD, WORD>> g_all_ids;  // every gamepad's VID/PID
constexpr int kKeyboard = -2;
int g_active = -1;        // index into g_pads, kKeyboard, or -1 (none)
DWORD g_packet = 0;
PadState g_out = {};      // the driving pad's last state, as handed out
WORD g_want_low = 0, g_want_high = 0;
std::vector<std::string> g_disabled;  // lower case
Family g_family = kXbox;  // button names: the last pad that drove

// KeyboardPad: driving keys feed the hub as one more controller, so the
// keyboard can take over (say, after a pad is unplugged mid-race).
bool g_kb_on = false;
enum KbKey { kSteerLeft, kSteerRight, kLeanForward, kLeanBack, kThrottle, kBrake, kStunt1, kStunt2, kKbKeys };
int g_kb_vk[kKbKeys] = {};
PadState g_kb = {};       // keyboard as a pad
float g_kb_x = 0, g_kb_y = 0;  // ramped steering / lean, -1..1
Uint64 g_kb_ns = 0;
DWORD g_kb_touches = 0;

// Other joysticks (wheels, flight sticks), opened only so the menu screens
// can read their hat and buttons; SDL reads them without taking them from
// the game's DirectInput.
struct Stick {
  SDL_JoystickID id = 0;
  SDL_Joystick* j = nullptr;
  std::string name;
  bool axes_centred = false;  // axes 0 / 1 rest in the middle (not pedals)
  StickState st = {-1, 0};
};
std::vector<Stick> g_sticks;

struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
};

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void ParseDisabled(const char* text) {
  std::string cur;
  auto flush = [&] {
    size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
    if (a != std::string::npos) g_disabled.push_back(Lower(cur.substr(a, b - a + 1)));
    cur.clear();
  };
  for (const char* p = text; *p; ++p) {
    if (*p == ',' || *p == ';') flush();
    else cur += *p;
  }
  flush();
}

bool IsDisabled(const std::string& name, WORD vid, WORD pid) {
  char ids[16];
  std::snprintf(ids, sizeof ids, "%04x:%04x", vid, pid);
  const std::string n = Lower(name);
  for (const std::string& d : g_disabled) {
    if (d == ids) return true;
    if (d.size() >= 3 && n.find(d) != std::string::npos) return true;
  }
  return false;
}

const char* TypeName(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_GAMEPAD_TYPE_PS3: return "PS3";
    case SDL_GAMEPAD_TYPE_PS4: return "PS4";
    case SDL_GAMEPAD_TYPE_PS5: return "PS5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
    default: return "unknown";
  }
}

Family FamilyOf(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return kXbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return kNintendo;
    default: return kGeneric;
  }
}

SHORT Flip(Sint16 v) { return static_cast<SHORT>(v == -32768 ? 32767 : -v); }

PadState ReadPad(SDL_Gamepad* g) {
  PadState s = {};
  s.lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
  s.ly = Flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));  // SDL: down positive
  s.rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
  s.ry = Flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
  s.lt = static_cast<BYTE>(std::max<int>(0, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) * 255 / 32767);
  s.rt = static_cast<BYTE>(std::max<int>(0, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 255 / 32767);
  static const struct {
    SDL_GamepadButton b;
    WORD bit;
  } kMap[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},
      {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},
      {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
      {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
  };
  for (const auto& m : kMap)
    if (SDL_GetGamepadButton(g, m.b)) s.buttons |= m.bit;
  return s;
}

bool Same(const PadState& a, const PadState& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

float Mag(SHORT x, SHORT y) {
  const float fx = x / 32767.0f, fy = y / 32767.0f;
  return fx * fx + fy * fy;
}

// A deliberate touch: a new button press, a trigger pulled past a quarter or
// a stick pushed past half way (resting sticks drift a little; that isn't one).
bool Touched(const PadState& was, const PadState& now) {
  if (now.buttons & ~was.buttons) return true;
  if ((now.lt > 64 && was.lt <= 64) || (now.rt > 64 && was.rt <= 64)) return true;
  if (Mag(now.lx, now.ly) > 0.25f && Mag(was.lx, was.ly) <= 0.25f) return true;
  if (Mag(now.rx, now.ry) > 0.25f && Mag(was.rx, was.ry) <= 0.25f) return true;
  return false;
}

void StopRumble(Pad& p) {
  if (p.rumble_low || p.rumble_high) SDL_RumbleGamepad(p.g, 0, 0, 0);
  p.rumble_low = p.rumble_high = 0;
}

void SetActive(int i, const char* why) {
  if (i == g_active) return;
  if (g_active >= 0 && g_active < static_cast<int>(g_pads.size())) StopRumble(g_pads[g_active]);
  g_active = i;
  ++g_packet;
  if (i >= 0) g_family = FamilyOf(g_pads[i].type);
  if (i >= 0)
    patch::Log("hub: %s now drives (%s)", g_pads[i].name.c_str(), why);
  else if (i == kKeyboard)
    patch::Log("hub: the keyboard now drives (%s)", why);
  else
    patch::Log("hub: no controller drives (%s)", why);
}

// Most recently used enabled pad, or the first enabled one, or -1.
int Best() {
  int best = -1;
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    if (!g_pads[i].enabled) continue;
    if (best < 0 || g_pads[i].last_used > g_pads[best].last_used) best = i;
  }
  return best;
}

bool GameFocused() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (w) GetWindowThreadProcessId(w, &pid);
  return pid == GetCurrentProcessId();
}

// The keyboard as a pad: steering and lean ease in and out like the game's
// own key steering; while a stunt key is held, Up/Down also tilt the stick
// (the game's keyboard stunts use the arrows for direction).
PadState ReadKeyboard(Uint64 now) {
  PadState s = {};
  bool down[kKbKeys] = {};
  if (GameFocused())
    for (int k = 0; k < kKbKeys; ++k) down[k] = g_kb_vk[k] && (GetAsyncKeyState(g_kb_vk[k]) & 0x8000) != 0;
  const bool stunt = down[kStunt1] || down[kStunt2];
  const float want_x = (down[kSteerRight] ? 1.0f : 0.0f) - (down[kSteerLeft] ? 1.0f : 0.0f);
  float want_y = (down[kLeanForward] ? 1.0f : 0.0f) - (down[kLeanBack] ? 1.0f : 0.0f);
  if (stunt) want_y += (down[kThrottle] ? 1.0f : 0.0f) - (down[kBrake] ? 1.0f : 0.0f);
  want_y = std::max(-1.0f, std::min(1.0f, want_y));
  const float dt = g_kb_ns ? std::min(0.1f, static_cast<float>(now - g_kb_ns) / 1e9f) : 0.0f;
  g_kb_ns = now;
  auto ease = [&](float cur, float want) {
    // Full deflection in 0.15 s, back to centre in 0.1 s.
    const float rate = (want == 0.0f || (want > 0) != (cur > 0)) && cur != 0.0f ? 10.0f : 6.7f;
    const float step = rate * dt;
    if (std::fabs(want - cur) <= step) return want;
    return cur + (want > cur ? step : -step);
  };
  g_kb_x = ease(g_kb_x, want_x);
  g_kb_y = ease(g_kb_y, want_y);
  s.lx = static_cast<SHORT>(g_kb_x * 32767.0f);
  s.ly = static_cast<SHORT>(g_kb_y * 32767.0f);
  s.rt = down[kThrottle] ? 255 : 0;
  s.lt = down[kBrake] ? 255 : 0;
  if (down[kStunt1]) s.buttons |= XINPUT_GAMEPAD_X;  // button 3
  if (down[kStunt2]) s.buttons |= XINPUT_GAMEPAD_Y;  // button 4
  return s;
}

// A key that drives was pressed (not just held from before).
bool KeyboardTouched(const PadState& was, const PadState& now) {
  return (now.buttons & ~was.buttons) || (now.rt && !was.rt) || (now.lt && !was.lt) ||
         (now.lx && std::abs(now.lx) > std::abs(was.lx) && was.lx == 0) ||
         (now.ly && std::abs(now.ly) > std::abs(was.ly) && was.ly == 0);
}

int Vk(const char* name) {
  static const struct {
    const char* n;
    int vk;
  } kNames[] = {{"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN}, {"space", VK_SPACE},
                {"enter", VK_RETURN}, {"backspace", VK_BACK}, {"tab", VK_TAB}, {"shift", VK_SHIFT},
                {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"alt", VK_MENU}, {"insert", VK_INSERT},
                {"delete", VK_DELETE}, {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR},
                {"pagedown", VK_NEXT}, {"none", 0}};
  std::string n = Lower(name);
  n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return c == ' ' || c == '\t'; }), n.end());
  if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0])))
    return std::toupper(static_cast<unsigned char>(n[0]));
  if (n.size() >= 2 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) {
    const int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 12) return VK_F1 + f - 1;
  }
  if (n.size() == 7 && n.compare(0, 6, "numpad") == 0 && std::isdigit(static_cast<unsigned char>(n[6])))
    return VK_NUMPAD0 + (n[6] - '0');
  for (const auto& k : kNames)
    if (n == k.n) return k.vk;
  return -1;
}

void RefreshIds() {
  g_all_ids.clear();
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  for (int i = 0; ids && i < n; ++i)
    g_all_ids.emplace_back(SDL_GetGamepadVendorForID(ids[i]), SDL_GetGamepadProductForID(ids[i]));
  SDL_free(ids);
}

void Add(SDL_JoystickID id) {
  for (const Pad& p : g_pads)
    if (p.id == id) return;
  const SDL_JoystickType jt = SDL_GetJoystickTypeForID(id);
  const char* jname = SDL_GetGamepadNameForID(id);
  if (jt != SDL_JOYSTICK_TYPE_GAMEPAD && jt != SDL_JOYSTICK_TYPE_UNKNOWN) {
    patch::Log("hub: \"%s\" is not a gamepad (joystick type %d): left to the game as a DirectInput device",
               jname ? jname : "?", static_cast<int>(jt));
    return;
  }
  SDL_Gamepad* g = SDL_OpenGamepad(id);
  if (!g) {
    patch::Log("hub: could not open \"%s\": %s", jname ? jname : "?", SDL_GetError());
    return;
  }
  Pad p;
  p.id = id;
  p.g = g;
  const char* name = SDL_GetGamepadName(g);
  p.name = name && *name ? name : "Gamepad";
  p.vid = SDL_GetGamepadVendor(g);
  p.pid = SDL_GetGamepadProduct(g);
  p.type = SDL_GetGamepadType(g);
  p.enabled = !IsDisabled(p.name, p.vid, p.pid);
  p.st = ReadPad(g);
  const char* path = SDL_GetGamepadPath(g);
  patch::Log("hub: connected \"%s\" (%s, VID_%04X PID_%04X, %s%s)%s path %s", p.name.c_str(), TypeName(p.type), p.vid,
             p.pid, SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "rumble" : "no rumble",
             SDL_GetGamepadConnectionState(g) == SDL_JOYSTICK_CONNECTION_WIRELESS ? ", wireless" : "",
             p.enabled ? "" : " DISABLED by DisabledControllers", path ? path : "?");
  g_pads.push_back(p);
  if (g_active == -1 && p.enabled) SetActive(static_cast<int>(g_pads.size()) - 1, "connected");
}

void AddStick(SDL_JoystickID id) {
  for (const Pad& p : g_pads)
    if (p.id == id) return;
  for (const Stick& k : g_sticks)
    if (k.id == id) return;
  SDL_Joystick* j = SDL_OpenJoystick(id);
  if (!j) {
    patch::Log("hub: could not open joystick %u for the menus: %s", static_cast<unsigned>(id), SDL_GetError());
    return;
  }
  Stick k;
  k.id = id;
  k.j = j;
  const char* name = SDL_GetJoystickName(j);
  k.name = name && *name ? name : "Joystick";
  const int axes = SDL_GetNumJoystickAxes(j);
  k.axes_centred = axes >= 2 && std::abs(SDL_GetJoystickAxis(j, 0)) < 8000 && std::abs(SDL_GetJoystickAxis(j, 1)) < 8000;
  patch::Log("hub: joystick \"%s\" (%d axes, %d buttons, %d hats) read for the menus only%s", k.name.c_str(), axes,
             SDL_GetNumJoystickButtons(j), SDL_GetNumJoystickHats(j),
             k.axes_centred ? "" : " (axes 0 / 1 not centred: hat only)");
  g_sticks.push_back(k);
}

void RemoveStick(SDL_JoystickID id) {
  for (size_t i = 0; i < g_sticks.size(); ++i) {
    if (g_sticks[i].id != id) continue;
    patch::Log("hub: joystick \"%s\" disconnected", g_sticks[i].name.c_str());
    SDL_CloseJoystick(g_sticks[i].j);
    g_sticks.erase(g_sticks.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

StickState ReadStick(const Stick& k) {
  StickState st = {-1, 0};
  const Uint8 hat = SDL_GetNumJoystickHats(k.j) > 0 ? SDL_GetJoystickHat(k.j, 0) : SDL_HAT_CENTERED;
  if (hat & SDL_HAT_UP) st.dir = 0;
  else if (hat & SDL_HAT_RIGHT) st.dir = 1;
  else if (hat & SDL_HAT_DOWN) st.dir = 2;
  else if (hat & SDL_HAT_LEFT) st.dir = 3;
  else if (k.axes_centred) {
    const int x = SDL_GetJoystickAxis(k.j, 0), y = SDL_GetJoystickAxis(k.j, 1);  // y: down positive
    if (y < -24000) st.dir = 0;
    else if (x > 24000) st.dir = 1;
    else if (y > 24000) st.dir = 2;
    else if (x < -24000) st.dir = 3;
  }
  const int n = std::min(SDL_GetNumJoystickButtons(k.j), 32);
  for (int b = 0; b < n; ++b)
    if (SDL_GetJoystickButton(k.j, b)) st.buttons |= 1u << b;
  return st;
}

void Remove(SDL_JoystickID id) {
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    if (g_pads[i].id != id) continue;
    patch::Log("hub: disconnected \"%s\"", g_pads[i].name.c_str());
    SDL_CloseGamepad(g_pads[i].g);
    const bool was_active = i == g_active;
    g_pads.erase(g_pads.begin() + i);
    if (g_active > i) --g_active;
    if (was_active) {
      g_active = -1;
      SetActive(Best(), "the driving controller was unplugged");
      if (g_active == -1) {
        g_out = PadState{};
        ++g_packet;
      }
    }
    return;
  }
}

}  // namespace

bool Available() { return true; }

void SetKeyboard(bool on, const char* const keys[8]) {
  g_kb_on = on;
  static const char* const kWhat[kKbKeys] = {"KeySteerLeft", "KeySteerRight", "KeyLeanForward", "KeyLeanBack",
                                             "KeyThrottle",  "KeyBrake",      "KeyStunt1",      "KeyStunt2"};
  std::string list;
  for (int k = 0; k < kKbKeys; ++k) {
    int vk = Vk(keys[k]);
    if (vk < 0) {
      patch::Log("hub: %s=%s is not a key name I know; that key is off", kWhat[k], keys[k]);
      vk = 0;
    }
    g_kb_vk[k] = vk;
    char b[64];
    std::snprintf(b, sizeof b, "%s%s=%s", list.empty() ? "" : " ", kWhat[k], keys[k]);
    list += b;
  }
  patch::Log("hub: KeyboardPad=%d (%s)", on ? 1 : 0, list.c_str());
}

bool Init(bool log, const char* disabled) {
  InitializeCriticalSection(&g_cs);
  g_log = log;
  ParseDisabled(disabled ? disabled : "");
  // No SDL window exists, so SDL never thinks the game is in the background;
  // the hint makes sure. The joystick thread watches for hot-plugging.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) {
    patch::Log("hub: SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  int v = SDL_GetVersion();
  patch::Log("hub: SDL %d.%d.%d (%s), joystick drivers ready", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
             SDL_VERSIONNUM_MICRO(v), SDL_GetRevision());
  // Extra mappings: gamecontrollerdb.txt next to Mcm2.exe (optional).
  char db[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, db, MAX_PATH)) {
    if (char* slash = std::strrchr(db, '\\')) {
      std::snprintf(slash + 1, static_cast<size_t>(db + MAX_PATH - slash - 1), "gamecontrollerdb.txt");
      if (GetFileAttributesA(db) != INVALID_FILE_ATTRIBUTES) {
        const int n = SDL_AddGamepadMappingsFromFile(db);
        patch::Log("hub: %s: %d mappings added%s%s", db, n, n < 0 ? ": " : "", n < 0 ? SDL_GetError() : "");
      }
    }
  }
  if (!g_disabled.empty()) {
    std::string list;
    for (const std::string& d : g_disabled) list += (list.empty() ? "" : ", ") + d;
    patch::Log("hub: DisabledControllers: %s", list.c_str());
  }
  Lock lock;
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  for (int i = 0; ids && i < n; ++i) Add(ids[i]);
  SDL_free(ids);
  ids = SDL_GetJoysticks(&n);
  for (int i = 0; ids && i < n; ++i) AddStick(ids[i]);
  SDL_free(ids);
  RefreshIds();
  patch::Log("hub: %d gamepad(s) at start%s", static_cast<int>(g_pads.size()),
             g_pads.empty() ? " (plugging one in later works)" : "");
  g_ready = true;
  return true;
}

void Pump() {
  if (!g_ready) return;
  SDL_PumpEvents();
  SDL_Event e;
  Lock lock;
  bool changed_list = false;
  std::vector<SDL_JoystickID> joysticks_added;
  while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
      Add(e.gdevice.which);
      changed_list = true;
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
      Remove(e.gdevice.which);
      changed_list = true;
    } else if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
      joysticks_added.push_back(e.jdevice.which);  // after the gamepad events of this pump
    } else if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
      RemoveStick(e.jdevice.which);
    }
  }
  for (SDL_JoystickID id : joysticks_added)
    AddStick(id);  // not if it was opened as a gamepad
  for (Stick& k : g_sticks) k.st = ReadStick(k);
  if (changed_list) RefreshIds();
  const Uint64 now = SDL_GetTicksNS();
  if (g_kb_on) {
    const PadState k = ReadKeyboard(now);
    if (KeyboardTouched(g_kb, k)) {
      ++g_kb_touches;
      if (g_active != kKeyboard) SetActive(kKeyboard, "a driving key was pressed");
    }
    g_kb = k;
  }
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    Pad& p = g_pads[i];
    const PadState s = ReadPad(p.g);
    if (Touched(p.st, s)) {
      p.last_used = now;
      ++p.presses;
      if (p.enabled && i != g_active) SetActive(i, "it was used");
    }
    p.st = s;
  }
  if (g_active == kKeyboard && !Same(g_kb, g_out)) {
    g_out = g_kb;
    ++g_packet;
  }
  if (g_active >= 0) {
    Pad& a = g_pads[g_active];
    if (!Same(a.st, g_out)) {
      g_out = a.st;
      ++g_packet;
    }
    // Rumble: resend while running so SDL's duration never runs out.
    const bool change = a.rumble_low != g_want_low || a.rumble_high != g_want_high;
    const bool running = g_want_low || g_want_high;
    if (change || (running && now - a.rumble_sent > 500000000ull)) {
      SDL_RumbleGamepad(a.g, g_want_low, g_want_high, running ? 1000 : 0);
      a.rumble_low = g_want_low;
      a.rumble_high = g_want_high;
      a.rumble_sent = now;
    }
  }
}

bool Read(PadState* out, DWORD* packet) {
  if (!g_ready) {
    *out = PadState{};
    *packet = 0;
    return false;
  }
  Lock lock;
  *out = g_active != -1 ? g_out : PadState{};
  *packet = g_packet;
  return g_active != -1;
}

bool ReadAll(std::vector<PadState>* out) {
  out->clear();
  if (!g_ready) return false;
  Lock lock;
  for (const Pad& p : g_pads)
    if (p.enabled) out->push_back(p.st);
  return true;
}

bool ReadSticks(std::vector<StickState>* out) {
  out->clear();
  if (!g_ready) return false;
  Lock lock;
  for (const Stick& k : g_sticks) out->push_back(k.st);
  return true;
}

void Rumble(WORD low, WORD high) {
  if (!g_ready) return;
  Lock lock;
  g_want_low = low;
  g_want_high = high;
}

std::vector<std::pair<WORD, WORD>> GamepadIds() {
  if (!g_ready) return {};
  Lock lock;
  return g_all_ids;
}

Family ActiveFamily() {
  if (!g_ready) return kXbox;
  Lock lock;
  return g_family;
}

std::string ActiveName() {
  if (!g_ready) return "";
  Lock lock;
  return g_active >= 0 ? g_pads[g_active].name : g_active == kKeyboard ? "Keyboard" : "";
}

int Count() {
  if (!g_ready) return 0;
  Lock lock;
  int n = 0;
  for (const Pad& p : g_pads) n += p.enabled ? 1 : 0;
  return n;
}

void LogSummary() {
  if (!g_ready) return;
  Lock lock;
  if (g_kb_on)
    patch::Log("hub: %s\"Keyboard\": lx %d ly %d lt %u rt %u buttons 0x%04x, %lu touches",
               g_active == kKeyboard ? "* " : "  ", g_kb.lx, g_kb.ly, g_kb.lt, g_kb.rt, g_kb.buttons, g_kb_touches);
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    const Pad& p = g_pads[i];
    patch::Log("hub: %s\"%s\" %s: lx %d ly %d rx %d ry %d lt %u rt %u buttons 0x%04x, %lu touches, rumble %u/%u",
               i == g_active ? "* " : "  ", p.name.c_str(), p.enabled ? "enabled" : "disabled", p.st.lx, p.st.ly,
               p.st.rx, p.st.ry, p.st.lt, p.st.rt, p.st.buttons, p.presses, p.rumble_low, p.rumble_high);
  }
}

}  // namespace hub
