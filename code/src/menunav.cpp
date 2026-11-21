// MenuNav=1: the game's own menu screens (main menu, Options, the event
// screens, ...) work with the keyboard and controllers as well as the mouse,
// and look exactly as they do.
//
// It moves the game's own mouse cursor onto the chosen control and makes it
// the control under the cursor the way the game does (its own focus
// picture, sound and tool tip), and clicks it with the calls the game's own
// hotkeys (KeyBind) make, at the start of the game's next tick (outside the
// menu code, where a click may close the screen). Neither an injected mouse
// button nor an injected KeyBind key works: the game passes an input
// device's events to the menus only while that device's own cursor is the
// active one (0x4890f0), and touching a controller makes the game's own
// joystick cursor the active one.
//   - Arrows / d-pad / left stick (wheels and joysticks: hat or stick) move
//     the cursor to the nearest control in that direction.
//   - Enter / A (joystick button 1) clicks the control under the cursor.
//   - Esc / B (joystick button 2) clicks the screen's Back / Cancel button.
//   - A list box's rows are places of their own (A / Enter picks the row).
//   - When a click of ours opens a pull-down panel, the cursor goes to the
//     panel's first control.
//   - The mouse still works as before; it carries on from where the cursor is.
// Keys the screen itself uses (its controls' KeyBind: Enter / Esc on message
// boxes, the replay player's arrows) are left to the game.
//
// Game facts (Mcm2.exe):
//   - GUI manager: [[[0x5743ac] + 0x56c] + 0x2c]; its dialogs are found with
//     the tree iterator (0x46a880 / 0x46a980 / 0x46a970, class "UIDialog").
//     Dialog +0x148: modal. Controls: iterator over [dialog + 0x7f3c],
//     class "UIControl".
//   - Tree node: +0x8 previous, +0xc next, +0x10 first child, +0x14 parent;
//     +0x25 bit 0 shown, bit 3 off (not drawn / no input).
//   - Control +0x70: shown (0x471590 shows / hides, with its animation);
//     +0x68 animation (0 none, 1 coming in, 2 going out); +0x18c the control
//     it is anchored to (RelAnchor; the main menu's pull-down panels). The
//     game takes input on a control only while it and every control it is
//     anchored to are shown and not animating (0x471d30, 0x472070).
//   - KeyBind click (UIButton etc. key handler 0x472f00): enabled (+0x6c)
//     and shown, then 0x4886c0(ui, control, 0, 1) (makes it the control
//     under the cursor), then the control's vtable +0xdc (press) and +0xe0
//     (release = click) with (0, 0). ui = [[control + 0xb8] + 0x34].
//   - 0x488790(ui, control, 0): the control under the cursor (+0x1d8);
//     the old one gets vtable +0x80 (leave), the new one +0x7c (enter).
//   - Control: vtable = its class; rect +0x2c..+0x38 (left, top, right,
//     bottom) in 640x480 menu pixels; +0x6c enabled; +0xf4 name; +0x1d8 the
//     key that clicks it (KeyBind, a DirectInput key code, 0 = none).
//   - GUICursor (vtable 0x558f88) update 0x486100 -> 0x43fce0: the position
//     (+0x54, +0x58) comes from two input axes [+0x3c] / [+0x40] whose value
//     (float, +0x24) the mouse moves; setting the value moves the cursor.
#include "menunav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <string>
#include <vector>

#include "config.h"
#include "framerate.h"
#include "pad_hub.h"
#include "patch.h"
#include "ui/ui.h"

namespace {

constexpr uintptr_t kGame = 0x5743AC;
constexpr uintptr_t kIterInit = 0x46A880, kIterNext = 0x46A980, kIterEnd = 0x46A970;
using IterInitFn = void(__fastcall*)(void* it, void* edx, uintptr_t root, int mode, const char* cls);
using IterNextFn = uintptr_t(__fastcall*)(void* it, void* edx);
using IterEndFn = void(__fastcall*)(void* it, void* edx);

// Control classes (vtables) that take clicks.
struct Kind {
  uintptr_t vtable;
  const char* name;
};
const Kind kKinds[] = {
    {0x557D98, "button"},   {0x5582C8, "checkbox"}, {0x5585E0, "radio"},  {0x558B10, "dropdown"},
    {0x557EA0, "edit box"}, {0x5581B8, "list"},     {0x5580B0, "slider"},   {0x557FA8, "scroll arrow"},
};
const char* KindOf(uintptr_t vt) {
  for (const Kind& k : kKinds)
    if (k.vtable == vt) return k.name;
  return nullptr;
}

// Hover: ui's control under the cursor (0x488790).
constexpr uintptr_t kSetHover = 0x488790;
using SetHoverFn = int(__fastcall*)(uintptr_t ui, void* edx, uintptr_t control, uintptr_t* old);

// KeyBind click calls (0x472f00).
constexpr uintptr_t kSetCapture = 0x4886C0;
using SetCaptureFn = int(__fastcall*)(uintptr_t ui, void* edx, uintptr_t control, uintptr_t* old, int now);
using PressFn = int(__fastcall*)(uintptr_t control, void* edx, int a, int b);
constexpr unsigned kDikEsc = 0x01, kDikEnter = 0x1C, kDikPadEnter = 0x9C;
constexpr unsigned kDikUp = 0xC8, kDikDown = 0xD0, kDikLeft = 0xCB, kDikRight = 0xCD;

enum Action { kNone = -1, kUp = 0, kRight, kDown, kLeft, kSelect, kBack };
const char* const kActionNames[] = {"up", "right", "down", "left", "select", "back"};

// A place the cursor can go: a control, or one row of a list box.
struct Control {
  uintptr_t p = 0;   // the game's control
  uintptr_t id = 0;  // p, or p + 1 + row for a list row (controls are far apart)
  int row = -1;
  uintptr_t dialog = 0;
  const char* kind = "";
  std::string name;
  RECT r{};
  int key = 0;  // KeyBind
  float cx() const { return (r.left + r.right) * 0.5f; }
  float cy() const { return (r.top + r.bottom) * 0.5f; }
  // Where the cursor goes: the middle, but for a line of text (an event
  // name, a list row; the game right-aligns them) just inside its right
  // end, so the pointer sits after the text rather than over it.
  bool text_line() const { return row >= 0 || (r.right - r.left >= 3 * (r.bottom - r.top) && r.bottom - r.top <= 24); }
  float aim_x() const { return text_line() ? static_cast<float>(r.right - 3) : cx(); }
  float aim_y() const { return cy(); }
};

struct State {
  bool on = false;
  CRITICAL_SECTION cs;
  // Keys from the game's reads (any thread), handled at the next frame.
  struct Key {
    unsigned dik;
    bool down;
  };
  std::vector<Key> keys;
  bool key_down[256] = {};
  // Controllers.
  WORD pad_prev[8] = {};
  DWORD stick_prev[8] = {};
  int dir_prev = -1;  // held direction (any source)
  DWORD dir_since = 0, dir_last = 0;
  bool pads_seen = false;
  // The screen.
  std::vector<Control> controls;
  std::vector<int> bound_keys;  // KeyBind of every shown control
  unsigned long long signature = 0;
  uintptr_t focus = 0;
  std::vector<uintptr_t> known, appeared;
  std::vector<uintptr_t> overflow_dialogs;  // dialogs with a list longer than it shows  // places on the screen; new ones at the last change
  DWORD last_click = 0;
  // Cursor moves and clicks.
  bool move_wanted = false;
  float want_x = 0, want_y = 0;
  int click_wait = -1;  // frames since a click's cursor move (-1 = no click)
  std::string click_name;
  uintptr_t click_p = 0;
  uintptr_t click_ready = 0;  // to click at the next tick
  DWORD tick_at = 0;          // last MenuNavTick
  DWORD last_action = 0;
  bool axes_logged = false;
  unsigned frames = 0, actions = 0, clicks = 0;
  bool was_racing = false, was_ui = false;
} g;

void Lock() { EnterCriticalSection(&g.cs); }
void Unlock() { LeaveCriticalSection(&g.cs); }

bool Readable(uintptr_t p, size_t n) { return p && !IsBadReadPtr(reinterpret_cast<const void*>(p), n); }
uint8_t U8(uintptr_t p) { return *reinterpret_cast<const uint8_t*>(p); }
int I32(uintptr_t p) { return *reinterpret_cast<const int*>(p); }
uintptr_t Ptr(uintptr_t p) { return *reinterpret_cast<const uintptr_t*>(p); }

// Shown: this node and every parent up to (and including) `top`.
bool Shown(uintptr_t node, uintptr_t top) {
  for (int depth = 0; node && depth < 16; ++depth) {
    if (!Readable(node, 0x28)) return false;
    const uint8_t f = U8(node + 0x25);
    if (!(f & 1) || (f & 8)) return false;
    if (node == top) return true;
    node = Ptr(node + 0x14);
  }
  return true;
}

// Shown and taking input: the game's own test (0x471d30): shown (+0x70),
// and every control it is anchored to shown and not animating.
bool Live(uintptr_t c) {
  if (!I32(c + 0x70)) return false;
  uintptr_t a = Ptr(c + 0x18C);
  for (int depth = 0; a && depth < 8; ++depth) {
    if (!Readable(a, 0x190)) return false;
    if (!I32(a + 0x70) || I32(a + 0x68) != 0) return false;
    a = Ptr(a + 0x18C);
  }
  return true;
}

uintptr_t Manager() {
  const uintptr_t game = Ptr(kGame);
  if (!Readable(game, 0x570)) return 0;
  const uintptr_t ui = Ptr(game + 0x56C);
  if (!Readable(ui, 0x30)) return 0;
  const uintptr_t mgr = Ptr(ui + 0x2C);
  return Readable(mgr, 0x310) ? mgr : 0;
}

// The dialogs (cls "UIDialog") or controls under root, by a walk of the GUI
// tree (+0x10 child, +0xc next, +8 previous). Not the game's iterator
// (0x46a880): it counts open walks in a global the game checks before
// adding controls to a dialog.
template <typename F>
void Each(uintptr_t root, const char* cls, F f) {
  if (!Readable(root, 0x18)) return;
  const bool want_dialogs = std::strcmp(cls, "UIDialog") == 0;
  std::vector<uintptr_t> stack;
  std::unordered_set<uintptr_t> seen;
  stack.push_back(*reinterpret_cast<const uintptr_t*>(root + 0x10));
  for (int guard = 0; !stack.empty() && guard < 40000; ++guard) {
    const uintptr_t n = stack.back();
    stack.pop_back();
    if (!n || n == root || !Readable(n, 0x1EC)) continue;  // smallest GUI object (see menugame.cpp)
    if (!seen.insert(n).second) continue;
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(n);
    if (vt >= 0x550000 && vt < 0x560000) {
      bool dialog = false;
      if (Readable(n, 0x7F40)) {
        const char* name = reinterpret_cast<const char*>(n + 0x38);
        const size_t len = strnlen(name, 64);
        dialog = len > 4 && len < 64 && _stricmp(name + len - 4, ".dtm") == 0;
      }
      if (dialog == want_dialogs) f(n);
    }
    stack.push_back(*reinterpret_cast<const uintptr_t*>(n + 0x08));
    stack.push_back(*reinterpret_cast<const uintptr_t*>(n + 0x0C));
    stack.push_back(*reinterpret_cast<const uintptr_t*>(n + 0x10));
  }
}

// A list box's visible rows, one place each (0x478e20 hit-tests them the
// same way): list +0x1ec item count, +0x214 items (0x38 bytes each: +4 bit 0
// not selectable, +0xc row height), +0x1f0 first row shown, +0x200 rows
// shown, +0x224 rows selectable. False: no rows (the list is one place).
bool AddRows(const Control& list) {
  const uintptr_t c = list.p;
  if (!Readable(c, 0x228) || !I32(c + 0x224)) return false;
  const int count = I32(c + 0x1EC), first = I32(c + 0x1F0), shown = I32(c + 0x200);
  const uintptr_t items = Ptr(c + 0x214);
  if (count <= 0 || count > 4096 || first < 0 || shown <= 0 || !Readable(items, static_cast<size_t>(count) * 0x38)) return false;
  if (count > shown) g.overflow_dialogs.push_back(list.dialog);  // its scroll arrows are worth a stop
  bool any = false;
  int y = list.r.top;
  for (int i = first; i < count && i < first + shown; ++i) {
    const uintptr_t it = items + i * 0x38;
    const int h = I32(it + 0xC);
    if (h <= 0 || y + h > list.r.bottom + 2) break;
    if (!(U8(it + 4) & 1)) {
      Control k = list;
      k.row = i;
      k.id = c + 1 + i;
      k.kind = "list row";
      k.r = {list.r.left, y, list.r.right, y + h};
      char n[24];
      std::snprintf(n, sizeof n, "[%d]", i);
      k.name += n;
      g.controls.push_back(k);
      any = true;
    }
    y += h;
  }
  return any;
}

// Every control that takes clicks on the screens shown now (only the
// topmost modal dialog's, when there is one).
void Collect() {
  g.controls.clear();
  g.bound_keys.clear();
  g.overflow_dialogs.clear();
  const uintptr_t mgr = Manager();
  if (!mgr) return;
  std::vector<uintptr_t> dialogs;
  uintptr_t modal = 0;
  Each(mgr, "UIDialog", [&](uintptr_t d) {
    if (!Readable(d, 0x7F40) || !Shown(d, d)) return;
    dialogs.push_back(d);
    if (I32(d + 0x148)) modal = d;
  });
  for (uintptr_t d : dialogs) {
    if (modal && d != modal) continue;
    const uintptr_t list = Ptr(d + 0x7F3C);
    if (!Readable(list, 0x28)) continue;
    Each(list, "UIControl", [&](uintptr_t c) {
      if (!Readable(c, 0x1E0) || !Shown(c, d) || !Live(c)) return;
      const int key = I32(c + 0x1D8);
      if (key > 0 && key < 256) g.bound_keys.push_back(key);
      const char* kind = KindOf(Ptr(c));
      if (!kind || !I32(c + 0x6C)) return;
      Control k;
      k.p = c;
      k.dialog = d;
      k.kind = kind;
      k.r = {I32(c + 0x2C), I32(c + 0x30), I32(c + 0x34), I32(c + 0x38)};
      if (k.r.right - k.r.left < 4 || k.r.bottom - k.r.top < 4 || k.r.right <= 0 || k.r.bottom <= 0 || k.r.left >= 640 ||
          k.r.top >= 480)
        return;
      k.name.assign(reinterpret_cast<const char*>(c + 0xF4), strnlen(reinterpret_cast<const char*>(c + 0xF4), 64));
      k.key = key;
      k.id = c;
      if (!std::strcmp(kind, "list") && AddRows(k)) return;
      g.controls.push_back(k);
    });
  }
  // Scroll arrows (UIScrollCtl) only where a list has more rows than it
  // shows: otherwise they do nothing.
  g.controls.erase(std::remove_if(g.controls.begin(), g.controls.end(),
                                  [](const Control& k) {
                                    return !std::strcmp(k.kind, "scroll arrow") &&
                                           std::find(g.overflow_dialogs.begin(), g.overflow_dialogs.end(), k.dialog) ==
                                               g.overflow_dialogs.end();
                                  }),
                   g.controls.end());
  unsigned long long sig = 1469598103934665603ull;
  for (const Control& k : g.controls) sig = (sig ^ k.id ^ (static_cast<unsigned long long>(k.r.left) << 32) ^ k.r.top) * 1099511628211ull;
  if (sig != g.signature) {
    g.signature = sig;
    std::string list;
    char buf[96];
    for (const Control& k : g.controls) {
      std::snprintf(buf, sizeof buf, "%s%s (%s %ld,%ld %ldx%ld%s)", list.empty() ? "" : ", ", k.name.c_str(), k.kind, k.r.left,
                    k.r.top, k.r.right - k.r.left, k.r.bottom - k.r.top, k.key ? " key" : "");
      list += buf;
      if (list.size() > 1500) {
        list += ", ...";
        break;
      }
    }
    patch::Log("menunav: screen now has %u controls in %u dialog(s)%s: %s", static_cast<unsigned>(g.controls.size()),
               static_cast<unsigned>(dialogs.size()), modal ? " (modal)" : "", list.c_str());
    if (g.focus && std::none_of(g.controls.begin(), g.controls.end(), [](const Control& k) { return k.id == g.focus; }))
      g.focus = 0;
    // Places that weren't there before (a pull-down panel finished opening).
    g.appeared.clear();
    for (const Control& k : g.controls)
      if (std::find(g.known.begin(), g.known.end(), k.id) == g.known.end()) g.appeared.push_back(k.id);
    g.known.clear();
    for (const Control& k : g.controls) g.known.push_back(k.id);
  }
}

const Control* Find(uintptr_t id) {
  for (const Control& k : g.controls)
    if (k.id == id) return &k;
  return nullptr;
}

bool Bound(unsigned dik) { return std::find(g.bound_keys.begin(), g.bound_keys.end(), static_cast<int>(dik)) != g.bound_keys.end(); }

POINT CursorPos(uintptr_t cursor) { return POINT{I32(cursor + 0x54), I32(cursor + 0x58)}; }

const Control* Under(POINT p) {
  for (const Control& k : g.controls)
    if (PtInRect(&k.r, p)) return &k;
  return nullptr;
}

// The nearest control from (ox, oy) in a direction: along it at least a few
// pixels, scored by distance along it plus twice the sideways offset (none
// when the two overlap sideways).
const Control* Nearest(const RECT* from, float ox, float oy, int dir, uintptr_t skip) {
  const Control* best = nullptr;
  float best_score = 1e9f;
  for (const Control& k : g.controls) {
    if (k.id == skip) continue;
    const float dx = k.cx() - ox, dy = k.cy() - oy;
    float along, side;
    bool overlap;
    if (dir == kUp || dir == kDown) {
      along = dir == kUp ? -dy : dy;
      overlap = from && k.r.left < from->right && k.r.right > from->left;
      side = std::fabs(dx);
    } else {
      along = dir == kLeft ? -dx : dx;
      overlap = from && k.r.top < from->bottom && k.r.bottom > from->top;
      side = std::fabs(dy);
    }
    if (along < 4) continue;
    const float score = along + (overlap ? 0.0f : 2.0f * side);
    if (score < best_score) {
      best_score = score;
      best = &k;
    }
  }
  return best;
}

// The screen's way back: the control the game clicks for Esc, else a button
// called Back / Cancel.
const Control* BackControl() {
  for (const Control& k : g.controls)
    if (k.key == static_cast<int>(kDikEsc)) return &k;
  static const char* const kNames[] = {"Back", "ButCancel", "CancelButton", "ButCancelProfile", "ButExit"};
  for (const char* n : kNames)
    for (const Control& k : g.controls)
      if (_stricmp(k.name.c_str(), n) == 0) return &k;
  return nullptr;
}

void MoveTo(const Control& k, const char* why) {
  g.focus = k.id;
  g.move_wanted = true;
  g.want_x = std::floor(k.aim_x());
  g.want_y = std::floor(k.aim_y());
  patch::Log("menunav: %s -> %s (%s %ld,%ld %ldx%ld), cursor to %.0f,%.0f", why, k.name.c_str(), k.kind, k.r.left, k.r.top,
             k.r.right - k.r.left, k.r.bottom - k.r.top, g.want_x, g.want_y);
  // Light it up as the game does when the mouse goes over it (it may not
  // notice the cursor moving under it until the mouse itself moves).
  const uintptr_t owner = Ptr(k.p + 0xB8);
  const uintptr_t ui = Readable(owner, 0x38) ? Ptr(owner + 0x34) : 0;
  if (Readable(ui, 0x1E0) && Ptr(ui + 0x1D8) != k.p && !Ptr(ui + 0x1CC)) {
    const int ok = reinterpret_cast<SetHoverFn>(kSetHover)(ui, nullptr, k.p, nullptr);
    if (GetConfig().menu_log || !ok) patch::Log("menunav: %s under the cursor%s", k.name.c_str(), ok ? "" : ": the game said no");
  }
}

bool Clickable(const char* kind) {
  return !std::strcmp(kind, "button") || !std::strcmp(kind, "checkbox") || !std::strcmp(kind, "radio") ||
         !std::strcmp(kind, "dropdown") || !std::strcmp(kind, "list row") || !std::strcmp(kind, "scroll arrow");
}

void Click(const Control& k, const char* why) {
  MoveTo(k, why);
  if (!Clickable(k.kind)) {
    patch::Log("menunav: %s is a %s; it can't be clicked from the keyboard or a controller yet (the mouse still can)",
               k.name.c_str(), k.kind);
    return;
  }
  g.click_wait = 0;  // frames since the cursor was sent there
  g.click_name = k.name;
  g.click_p = k.id;
}

// Clicks a control with the calls the game's KeyBind code makes (0x472f00).
// A list row: the list's own mouse press and release (0x478e20) at the
// row's middle, as a click there does.
void ClickNow(uintptr_t id, const char* where) {
  Collect();
  const Control* k = Find(id);
  if (!k) {
    patch::Log("menunav: %s is no longer on the screen; no click", g.click_name.c_str());
    return;
  }
  const uintptr_t p = k->p;
  const uintptr_t owner = Ptr(p + 0xB8);
  const uintptr_t ui = Readable(owner, 0x38) ? Ptr(owner + 0x34) : 0;
  if (!Readable(ui, 0x1E0) || !I32(p + 0x6C) || !I32(p + 0x70)) {
    patch::Log("menunav: %s can't take a click now (ui %08x, enabled %d, shown %d)", k->name.c_str(),
               static_cast<unsigned>(ui), I32(p + 0x6C), I32(p + 0x70));
    return;
  }
  const std::string name = k->name;  // the click may change the screen (and the list)
  POINT at{static_cast<LONG>(k->cx()), static_cast<LONG>(k->cy())};
  const int arg = k->row >= 0 ? static_cast<int>(reinterpret_cast<uintptr_t>(&at)) : 0;
  ++g.clicks;
  g.last_click = GetTickCount();
  g.appeared.clear();
  patch::Log("menunav: click %s (%s)", name.c_str(), where);
  const uintptr_t vt = Ptr(p);
  reinterpret_cast<SetCaptureFn>(kSetCapture)(ui, nullptr, p, nullptr, 1);
  reinterpret_cast<PressFn>(Ptr(vt + 0xDC))(p, nullptr, 0, arg);
  reinterpret_cast<PressFn>(Ptr(vt + 0xE0))(p, nullptr, 0, arg);
  patch::Log("menunav: click %s done", name.c_str());
}

void Do(int action, const char* from, uintptr_t cursor) {
  ++g.actions;
  g.last_action = GetTickCount();
  Collect();
  const POINT cur = CursorPos(cursor);
  char why[96];
  std::snprintf(why, sizeof why, "%s (%s)", kActionNames[action], from);
  if (g.controls.empty()) {
    patch::Log("menunav: %s: no controls on this screen", why);
    return;
  }
  const Control* f = Find(g.focus);
  // The cursor was moved away (mouse) from the control: start from what is
  // under it now.
  if (f && !PtInRect(&f->r, cur)) f = Under(cur);
  if (action == kBack) {
    if (const Control* b = BackControl()) Click(*b, why);
    else patch::Log("menunav: %s: this screen has no Back / Cancel button", why);
    return;
  }
  if (action == kSelect) {
    if (!f) f = Under(cur);
    if (f) {
      if (std::strcmp(f->kind, "edit box") == 0 && std::strcmp(from, "keyboard") == 0) {
        patch::Log("menunav: %s: %s is an edit box; Enter is left to it", why, f->name.c_str());
        return;
      }
      Click(*f, why);
    } else {
      // Nothing under the cursor yet: the first press only picks a control.
      const Control* n = Nearest(nullptr, static_cast<float>(cur.x), static_cast<float>(cur.y), kDown, 0);
      MoveTo(n ? *n : g.controls.front(), why);
    }
    return;
  }
  const Control* to;
  if (f) {
    to = Nearest(&f->r, f->cx(), f->cy(), action, f->id);
  } else {
    // Nothing chosen yet: the control under the cursor, else the nearest one
    // from the cursor in that direction, else the first.
    to = Under(cur);
    if (!to) to = Nearest(nullptr, static_cast<float>(cur.x), static_cast<float>(cur.y), action, 0);
    if (!to) to = &g.controls.front();
  }
  if (to) MoveTo(*to, why);
  else if (GetConfig().menu_log) patch::Log("menunav: %s: nothing further that way from %s", why, f ? f->name.c_str() : "-");
}

// Controllers: 0 up, 1 right, 2 down, 3 left, -1 none; A / B as select / back.
void PollPads(std::vector<std::pair<int, const char*>>& acts, int& dir, const char*& dir_from) {
  std::vector<hub::PadState> pads;
  if (!hub::ReadAll(&pads)) {
    // No ControllerHub: XInput's four slots.
    struct XPad {
      DWORD packet;
      WORD buttons;
      BYTE lt, rt;
      SHORT lx, ly, rx, ry;
    };
    using GetFn = DWORD(WINAPI*)(DWORD, XPad*);
    static GetFn get = [] {
      for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
        if (HMODULE m = LoadLibraryA(dll))
          if (auto f = reinterpret_cast<GetFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")))) return f;
      return static_cast<GetFn>(nullptr);
    }();
    if (get)
      for (DWORD i = 0; i < 4; ++i) {
        XPad s{};
        if (get(i, &s) == ERROR_SUCCESS) pads.push_back(hub::PadState{s.lx, s.ly, s.rx, s.ry, s.lt, s.rt, s.buttons});
      }
  }
  for (size_t i = 0; i < pads.size() && i < 8; ++i) {
    const hub::PadState& s = pads[i];
    const WORD pressed = g.pads_seen ? static_cast<WORD>(s.buttons & ~g.pad_prev[i]) : 0;
    g.pad_prev[i] = s.buttons;
    int d = -1;
    if (s.buttons & 0x0001) d = 0;
    else if (s.buttons & 0x0008) d = 1;
    else if (s.buttons & 0x0002) d = 2;
    else if (s.buttons & 0x0004) d = 3;
    else if (s.ly > 20000) d = 0;
    else if (s.lx > 20000) d = 1;
    else if (s.ly < -20000) d = 2;
    else if (s.lx < -20000) d = 3;
    if (dir < 0 && d >= 0) {
      dir = d;
      dir_from = "controller";
    }
    if (pressed & 0x1000) acts.emplace_back(kSelect, "controller A");
    if (pressed & 0x2000) acts.emplace_back(kBack, "controller B");
  }
  std::vector<hub::StickState> sticks;
  if (hub::ReadSticks(&sticks)) {
    for (size_t i = 0; i < sticks.size() && i < 8; ++i) {
      const DWORD pressed = g.pads_seen ? sticks[i].buttons & ~g.stick_prev[i] : 0;
      g.stick_prev[i] = sticks[i].buttons;
      if (dir < 0 && sticks[i].dir >= 0) {
        dir = sticks[i].dir;
        dir_from = "joystick";
      }
      if (pressed & 1) acts.emplace_back(kSelect, "joystick button 1");
      if (pressed & 2) acts.emplace_back(kBack, "joystick button 2");
    }
  }
  g.pads_seen = true;
}

// Puts the cursor's input axes at (x, y): the update that follows makes it
// the cursor's position, and the mouse carries on from there.
bool SetCursor(uintptr_t cursor, float x, float y) {
  const uintptr_t ax = Ptr(cursor + 0x3C), ay = Ptr(cursor + 0x40);
  if (!Readable(ax, 0x28) || !Readable(ay, 0x28)) {
    if (!g.axes_logged) {
      g.axes_logged = true;
      patch::Log("menunav: the menu cursor has no input axes (%08x, %08x): it follows the Windows cursor; MenuNav can't move it",
                 static_cast<unsigned>(ax), static_cast<unsigned>(ay));
    }
    return false;
  }
  if (!g.axes_logged) {
    g.axes_logged = true;
    patch::Log("menunav: menu cursor %08x, axes %08x / %08x (now %.1f, %.1f)", static_cast<unsigned>(cursor),
               static_cast<unsigned>(ax), static_cast<unsigned>(ay), *reinterpret_cast<float*>(ax + 0x24),
               *reinterpret_cast<float*>(ay + 0x24));
  }
  *reinterpret_cast<float*>(ax + 0x24) = x;
  *reinterpret_cast<float*>(ay + 0x24) = y;
  return true;
}

}  // namespace

void MenuNavInstall() {
  const Config& c = GetConfig();
  if (!c.menu_nav) {
    patch::Log("menunav: off (MenuNav=0)");
    return;
  }
  if (!c.input_timing) {
    patch::Log("menunav: off: needs InputTiming=1 (it reads keys and gives clicks through it)");
    return;
  }
  InitializeCriticalSection(&g.cs);
  g.on = true;
  patch::Log("menunav: on: arrows / d-pad / stick move the menu cursor between controls, Enter / A click, Esc / B back");
}

bool MenuNavOn() { return g.on; }

void MenuNavKey(unsigned dik, bool down) {
  if (!g.on || dik >= 256) return;
  Lock();
  if (g.key_down[dik] != down) {  // the same change read two ways counts once
    g.key_down[dik] = down;
    if (g.keys.size() < 64) g.keys.push_back({dik, down});
  }
  Unlock();
}

void MenuNavTick() {
  if (!g.on) return;
  g.tick_at = GetTickCount();
  if (const uintptr_t p = g.click_ready) {
    g.click_ready = 0;
    ClickNow(p, "at the start of the game's tick");
  }
}

void MenuNavFrame(uintptr_t cursor) {
  if (!g.on || !Readable(cursor, 0x60)) return;
  ++g.frames;
  std::vector<State::Key> keys;
  Lock();
  keys.swap(g.keys);
  Unlock();
  // Not while a MenuMaX screen is open (it takes the input) or a race runs
  // (arrows steer, buttons do stunts).
  const bool racing = RaceRunning(), screen = ui::Active();
  if (racing != g.was_racing || screen != g.was_ui) {
    patch::Log("menunav: %s", racing ? "race running: off" : screen ? "MenuMaX screen open: off" : "menus: on");
    g.was_racing = racing;
    g.was_ui = screen;
    g.dir_prev = -1;
    g.click_wait = -1;
    g.click_ready = 0;
    g.move_wanted = false;
  }
  if (racing || screen) {
    g.pads_seen = false;  // buttons held now are not presses later
    return;
  }
  // Keys and controllers -> actions.
  std::vector<std::pair<int, const char*>> acts;
  int dir = -1;
  const char* dir_from = "keyboard";
  if (!keys.empty()) Collect();  // the screen's own key binds
  for (const State::Key& k : keys) {
    int a = kNone;
    if (k.dik == kDikUp) a = kUp;
    else if (k.dik == kDikDown) a = kDown;
    else if (k.dik == kDikLeft) a = kLeft;
    else if (k.dik == kDikRight) a = kRight;
    else if (k.dik == kDikEnter || k.dik == kDikPadEnter) a = kSelect;
    else if (k.dik == kDikEsc) a = kBack;
    if (a == kNone) continue;
    if (Bound(k.dik)) {
      if (k.down) patch::Log("menunav: key %02x is this screen's own (KeyBind); left to the game", k.dik);
      continue;
    }
    if (a <= kLeft) {
      if (!k.down && g.dir_prev == a) g.dir_prev = -1;
      if (k.down) {
        acts.emplace_back(a, "keyboard");
        g.dir_prev = a;
        g.dir_since = g.dir_last = GetTickCount();
      }
    } else if (k.down) {
      acts.emplace_back(a, "keyboard");
    }
  }
  // Controllers (a held direction is a press, then repeats).
  static const unsigned kDirDik[4] = {kDikUp, kDikRight, kDikDown, kDikLeft};
  const bool keyboard_dir = g.dir_prev >= 0 && g.key_down[kDirDik[g.dir_prev]];
  static int pad_dir_prev = -1;
  PollPads(acts, dir, dir_from);
  if (dir != pad_dir_prev) {
    pad_dir_prev = dir;
    if (dir >= 0) {
      acts.emplace_back(dir, dir_from);
      g.dir_prev = dir;
      g.dir_since = g.dir_last = GetTickCount();
    } else if (!keyboard_dir) {
      g.dir_prev = -1;
    }
  }
  // Held direction: repeats after 400 ms, every 140 ms.
  const DWORD now = GetTickCount();
  const bool held = g.dir_prev >= 0 && (keyboard_dir || pad_dir_prev == g.dir_prev);
  if (held && now - g.dir_since > 400 && now - g.dir_last >= 140) {
    g.dir_last = now;
    acts.emplace_back(g.dir_prev, "held");
  }
  for (const auto& a : acts) Do(a.first, a.second, cursor);
  // Cursor and click.
  if (g.move_wanted) {
    g.move_wanted = false;
    SetCursor(cursor, g.want_x, g.want_y);
  }
  if (g.click_wait >= 0 && !g.click_ready) {
    // Click once the cursor has been on the control for a frame (it lights
    // up first), or after 10 frames whatever happened.
    const POINT p = CursorPos(cursor);
    const bool there = std::fabs(p.x - g.want_x) < 1.5f && std::fabs(p.y - g.want_y) < 1.5f;
    ++g.click_wait;
    if ((there && g.click_wait >= 3) || g.click_wait >= 10) {
      g.click_wait = -1;
      if (!there)
        patch::Log("menunav: cursor at %ld,%ld, not at %.0f,%.0f as asked; clicking anyway", p.x, p.y, g.want_x, g.want_y);
      if (now - g.tick_at < 500) {
        g.click_ready = g.click_p;  // at the start of the next tick
      } else {
        ClickNow(g.click_p, "no tick hook: from the menu cursor update");
      }
    }
  }
  // A click of ours opened a pull-down panel (Single Player, Multiplayer,
  // User Profiles): once its controls take input (its animation is over),
  // the cursor goes to the first of them (reading order).
  if (g.last_click && now - g.last_click < 4000 && g.frames % 4 == 0 && !g.click_ready && g.click_wait < 0) {
    Collect();
    const Control* first = nullptr;
    auto band = [](const Control& k) { return static_cast<int>(k.cy()) / 24; };
    for (uintptr_t id : g.appeared) {
      const Control* k = Find(id);
      if (!k || !Ptr(k->p + 0x18C)) continue;  // only controls anchored to a panel
      if (!first || band(*k) < band(*first) || (band(*k) == band(*first) && k->cx() < first->cx())) first = k;
    }
    g.appeared.clear();
    if (first) {
      g.last_click = 0;
      MoveTo(*first, "panel opened");
    }
  } else if (g.last_click && now - g.last_click >= 4000) {
    g.last_click = 0;
  }
  // After an action, where the cursor really went (for the log).
  if (g.last_action && now - g.last_action > 100 && now - g.last_action < 100 + 40) {
    const POINT p = CursorPos(cursor);
    patch::Log("menunav: cursor now at %ld,%ld", p.x, p.y);
    g.last_action = 0;
  }
}
