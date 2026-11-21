// MenuMaX: see ui.h.
#include "ui.h"

#include <RmlUi/Core.h>

#include <cmath>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../config.h"
#include "../pad_hub.h"
#include "../mode_prompt.h"
#include "../patch.h"
#include "../dd/backend.h"
#include "../framerate.h"
#include "../menugame.h"
#include "render_d3d11.h"
#include "render_d3d9.h"

// Counts the game's dialog shows (see Install).
extern "C" {
volatile long g_max_dialog_shows = 0;
void MaxDialogShowHook();
}
asm(".text\n"
    ".globl _MaxDialogShowHook\n"
    "_MaxDialogShowHook:\n"
    "  lock incl _g_max_dialog_shows\n"
    "  subl $0x10, %esp\n"
    "  pushl %ebx\n"
    "  movl 0x20(%esp), %ebx\n"
    "  pushl $0x486A08\n"
    "  ret\n");

// The game's dialog constructor (0x46ace0, this = ecx, (1, "name.dtm")): the
// game can draw and present a new dialog before it shows it (0x486a00), so
// presents are held from here until the copy is up (see HoldPresent).
extern "C" {
void __cdecl MaxDialogNew(const char* name);
void MaxDialogNewHook();
}
asm(".text\n"
    ".globl _MaxDialogNewHook\n"
    "_MaxDialogNewHook:\n"
    "  pushal\n"
    "  pushl 0x28(%esp)\n"  // name: [esp+8] at entry, + 32 (pushal)
    "  call _MaxDialogNew\n"
    "  addl $4, %esp\n"
    "  popal\n"
    "  pushl $-1\n"
    "  pushl $0x550526\n"
    "  pushl $0x46ACE7\n"
    "  ret\n");

// menus.cpp: the Enhancements box brought up to date after a screen.
void MenusRefreshFromIni();

namespace ui {

namespace {

// ---------------------------------------------------------------- helpers

std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
std::string Narrow(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring GameDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  const size_t slash = s.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring(L".\\") : s.substr(0, slash + 1);
}
std::wstring UiDir() { return GameDir() + L"UI\\MaX\\"; }

// "1.500000" -> "1.5"; non-numbers unchanged.
std::string Tidy(const std::string& v) {
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (v.empty() || !end || *end) return v;
  char b[32];
  std::snprintf(b, sizeof(b), "%g", d);
  return b;
}
bool IsZero(const std::string& v) {
  if (v.empty()) return true;
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (end && !*end) return d == 0;
  return _stricmp(v.c_str(), "off") == 0 || _stricmp(v.c_str(), "false") == 0;
}

// ---------------------------------------------------------------- RmlUi interfaces

class System : public Rml::SystemInterface {
 public:
  System() {
    QueryPerformanceFrequency(&freq_);
    QueryPerformanceCounter(&t0_);
  }
  double GetElapsedTime() override {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - t0_.QuadPart) / static_cast<double>(freq_.QuadPart);
  }
  bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
    const char* what = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? "error"
                       : type == Rml::Log::LT_WARNING                            ? "warning"
                       : type == Rml::Log::LT_INFO                               ? "info"
                                                                                 : "debug";
    if (type == Rml::Log::LT_DEBUG) return true;
    if (++logged_ <= 300) patch::Log("MenuMaX: RmlUi %s: %s", what, message.c_str());
    if (logged_ == 300) patch::Log("MenuMaX: RmlUi: no more messages are logged this run");
    return true;
  }

 private:
  LARGE_INTEGER freq_{}, t0_{};
  unsigned logged_ = 0;
};

// Files by UTF-8 path, so a game folder with any name works.
class Files : public Rml::FileInterface {
 public:
  Rml::FileHandle Open(const Rml::String& path) override {
    FILE* f = _wfopen(Widen(path).c_str(), L"rb");
    if (!f && ++misses_ <= 50) patch::Log("MenuMaX: file not found: %s", path.c_str());
    return reinterpret_cast<Rml::FileHandle>(f);
  }
  void Close(Rml::FileHandle file) override { std::fclose(reinterpret_cast<FILE*>(file)); }
  size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
    return std::fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
  }
  bool Seek(Rml::FileHandle file, long offset, int origin) override {
    return std::fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
  }
  size_t Tell(Rml::FileHandle file) override { return static_cast<size_t>(std::ftell(reinterpret_cast<FILE*>(file))); }

 private:
  unsigned misses_ = 0;
};

// ---------------------------------------------------------------- state

struct Setting {
  Rml::Element* el = nullptr;
  std::string key;       // ini key (or game control name)
  bool game = false;     // a control on the game's own page, not an ini key
  bool checkbox = false;
  std::string on_value;  // written when a checkbox is ticked
  std::string value;     // as shown now
};

// An element tied to a control of the game's (game="Name", menugame.h): it
// shows the control's state, text, greying and visibility, and passes the
// player's changes on to it. game-if="Name": shown only while that control is.
struct GameBind {
  Rml::Element* el = nullptr;
  std::string name;
  bool only_if = false;  // game-if
  enum Ui { kDiv, kCheckInput, kRange, kSelect, kTextInput, kList } ui = kDiv;
  Rml::Element* text_el = nullptr;  // where the control's text goes (none: kept)
  bool seen = false;
  mg::Info last;
  std::vector<std::string> rows;
  bool dragging = false;  // a slider being moved: its release is still to send
  DWORD changed_at = 0;   // the player's last change (the game's value is not shown for a moment)
  // The game dialog the control is looked for on: game-dialog="optgame" on
  // the element or one around it, else the screen's first (game-dialogs=).
  // Empty: any shown dialog.
  std::string scope_dtm;
  uintptr_t scope = 0;  // that dialog now (0: not shown)
  int list_top = -1, list_value = -2;  // a list's rows as last drawn
  // game-anim-in / game-anim-out="url url ...": the pictures of a pop-out
  // panel's show / hide animation, drawn frame by frame as the game plays it.
  std::vector<std::string> anim_in, anim_out;
  int anim_shown = -1;  // the picture drawn now: 0 none (the element's own), else 1 + frame (+ 1000 for hide)
  // A panel the game has just shown but not yet started animating (the game
  // can stall for seconds first, e.g. Multiplayer probing the network): the
  // frames it has waited for its animation. -1: not waiting.
  int anim_wait = -1;
  bool was_live = false;
  // The show animation's frame when it began: the game can leave its frame
  // counter on the last frame until the first tick (Multiplayer, before its
  // network check stalls the game for seconds). Hidden until it moves on.
  int prev_anim = 0, start_frame = -1;
  bool located = false;  // looked for in the whole GUI tree once (log)
  // game-window: the game's own picture of the control is shown here (drawn
  // again over the screen at the control's place), e.g. the track preview.
  bool window = false;
  bool window_logged = false;
  // Frames / time it has been shown for: the game draws the picture a frame
  // or more after the control shows (before that the game's frame there
  // still holds the last screen, e.g. the main menu's green flag key).
  int window_frames = 0;
  DWORD window_since = 0;
};

// <div game-dialog="optgame">: the part of a screen that copies one game
// dialog. Shown while the game shows that dialog, at the place the game put
// it.
struct DialogBox {
  Rml::Element* el = nullptr;
  std::string dtm;
  long left = -100000, top = -100000;
};

// MaXMenus: a screen in UI\MaX\menus\ copies one or more of the game's
// dialogs (<body game-dialogs="optbase optgame ...">). The screens shown
// follow the game's own dialogs, in the game's order.
struct Layer {
  std::string file;  // e.g. "options" (menus\options.rml)
  Rml::ElementDocument* doc = nullptr;
  std::vector<std::string> dialogs;  // game-dialogs
  bool opaque = false;               // covers the whole menu: nothing under it is shown
  std::string back;                  // game-back
};

struct State {
  std::recursive_mutex mu;  // input queue and open/close requests
  bool installed = false;
  bool failed = false;   // RmlUi could not start: never again this run
  bool inited = false;
  System* system = nullptr;
  Files* files = nullptr;
  Renderer* render = nullptr;  // r11 or r9, by the renderer running
  RenderD3D11* r11 = nullptr;
  RenderD3D9* r9 = nullptr;
  Rml::Context* context = nullptr;
  Rml::ElementDocument* doc = nullptr;
  std::string want_open;  // set by Open, done at the next present
  std::string open_name;  // the screen shown
  bool want_close = false;
  bool active = false;    // input is taken (set as soon as Open is asked)
  // Live reload.
  unsigned long long signature = 0;
  DWORD last_watch = 0;
  // Settings on the open screen.
  std::vector<Setting> settings;
  std::vector<GameBind> game;
  bool updating = false;     // the game's values being put into elements: not changes
  std::string back_control;  // game-back="Back": Esc / B clicks it (the game closes the page)
  std::string back_dtm;      // the dialog game-back's control is on
  std::vector<DialogBox> boxes;
  // MaXMenus.
  bool mirror = false;       // g.doc is the top of `layers` (not a screen opened by name)
  DWORD race_mirror_at = 0;  // the game's dialogs last looked at in a race
  long dialog_shows_seen = 0;  // g_max_dialog_shows when last looked
  // A copied game dialog just made (0x46ace0): presents held until its copy
  // is up (at most 250 ms), so the game's own never reaches the screen.
  std::string hold_dtm;
  DWORD hold_since = 0;
  int held_frames = 0;
  long holds = 0;
  std::vector<Layer> layers;  // bottom first
  std::map<std::string, std::string> index;  // game dialog -> screen file
  std::map<std::string, std::pair<bool, std::vector<std::string>>> index_info;  // file -> opaque, dialogs
  bool index_read = false;
  std::vector<mg::Dialog> dialogs;  // the game's, this frame
  std::string missing;       // the game dialog shown that no screen copies (logged once)
  // The game waits for a key / button of its own (Options > Controls
  // remapping: OptCont +0x7f70): input goes to the game meanwhile.
  bool capture = false;
  std::map<std::string, std::string> pending;  // key -> value not yet saved
  DWORD last_change = 0;
  bool binding = false;  // values being put into controls: not changes
  // Input.
  struct Key {
    Rml::Input::KeyIdentifier id;
    bool down;
  };
  std::vector<Key> keys;
  std::vector<Rml::Character> chars;  // typed text (for edit boxes)
  std::vector<std::pair<int, bool>> buttons;
  int wheel = 0;
  DWORD joy_pov = 0xFFFFFFFF, joy_buttons = 0;
  bool joy_seen = false;
  // Held direction (keys, d-pad, stick) repeats.
  Rml::Input::KeyIdentifier held = Rml::Input::KI_UNKNOWN;
  DWORD held_since = 0, held_last = 0;
  WORD pad_prev[8] = {};
  bool pads_from_hub = false;
  size_t pad_count = 0, stick_count = 0;
  DWORD stick_prev[8] = {};
  int pad_dir_prev = -1;
  // One press seen through two devices at once (a keyboard that is also an
  // XInput pad, the game reading the keyboard two ways) counts once.
  bool key_down[256] = {};
  Rml::Input::KeyIdentifier last_key = Rml::Input::KI_UNKNOWN;
  int last_key_source = -1;
  DWORD last_key_at = 0;
  unsigned dup_keys = 0;
  // The control that had focus last: a click on empty space or text keeps it.
  Rml::Element* last_focus = nullptr;
  bool shift = false;
  // Tooltips: the control with a title= under the pointer (only compared,
  // never used: it may be gone), and since when.
  const Rml::Element* tip_owner = nullptr;
  DWORD tip_since = 0;
  bool tip_logged = false;
  // Pointer.
  // The screen's own pointer (window pixels), moved by the mouse movement
  // kept from the game (its cursor stays where it was, so the page under the
  // screen does not react to it).
  bool pointer_set = false;
  float px = 0, py = 0;
  int dx = 0, dy = 0;  // movement not yet applied (DirectInput units)
  unsigned moves = 0;
  Rml::TextureHandle pointer_tex = 0;
  Rml::Vector2i pointer_dims;
  bool pointer_tried = false;
  int mouse_x = -1, mouse_y = -1;
  // Statistics (logged every few seconds while open).
  unsigned frames = 0, key_events = 0, clicks = 0, saves = 0, reloads = 0;
  DWORD last_report = 0, opened_at = 0;
  DWORD dump_at = 0;  // when to log the open screens' elements (0: not)
};
State g;

void Report(bool force);
void FlushPending(const char* why);

// ---------------------------------------------------------------- XInput

struct XPad {
  DWORD packet;
  WORD buttons;
  BYTE lt, rt;
  SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XPad*);
XInputGetStateFn LoadXInput() {
  static XInputGetStateFn fn = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        if (auto f = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")))) {
          patch::Log("MenuMaX: controllers through %s", dll);
          return f;
        }
      }
    }
    patch::Log("MenuMaX: no XInput DLL; controllers only through DirectInput");
    return static_cast<XInputGetStateFn>(nullptr);
  }();
  return fn;
}

// ---------------------------------------------------------------- settings

void Collect(Rml::Element* e, std::vector<Rml::Element*>& out) {
  if (e->HasAttribute("ini") || e->HasAttribute("game") || e->HasAttribute("game-if") || e->HasAttribute("ini-text") ||
      e->HasAttribute("enable-if") || e->HasAttribute("game-dialog"))
    out.push_back(e);
  for (int i = 0; i < e->GetNumChildren(); ++i) Collect(e->GetChild(i), out);
}

std::string CurrentValue(const std::string& key, const std::string& def) {
  auto it = g.pending.find(key);
  if (it != g.pending.end()) return it->second;
  std::string v;
  if (ReadSetting(key.c_str(), &v) && !v.empty()) return Tidy(v);
  return def;
}

void SetControl(Setting& s, const std::string& v) {
  s.value = v;
  if (s.checkbox) {
    if (IsZero(v)) s.el->RemoveAttribute("checked");
    else s.el->SetAttribute("checked", "");
  } else if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(s.el)) {
    fc->SetValue(v);
  }
}

// <span ini-text="Key" zero="Off" suffix="x"> shows a setting's value.
void UpdateTexts() {
  std::vector<Rml::Element*> els;
  if (!g.doc) return;
  Collect(g.doc, els);
  for (Rml::Element* e : els) {
    if (e->HasAttribute("ini-text")) {
      const std::string key = e->GetAttribute<Rml::String>("ini-text", "");
      const std::string v = CurrentValue(key, e->GetAttribute<Rml::String>("default", ""));
      std::string text = IsZero(v) && e->HasAttribute("zero") ? e->GetAttribute<Rml::String>("zero", "")
                                                              : v + e->GetAttribute<Rml::String>("suffix", "");
      if (e->GetInnerRML() != text) e->SetInnerRML(text);
    }
    // enable-if="Key!=0" / "Key=1": greyed (disabled) otherwise.
    if (e->HasAttribute("enable-if")) {
      const std::string cond = e->GetAttribute<Rml::String>("enable-if", "");
      const size_t ne = cond.find("!="), eq = cond.find('=');
      bool on = true;
      if (ne != std::string::npos) on = Tidy(CurrentValue(cond.substr(0, ne), "0")) != Tidy(cond.substr(ne + 2));
      else if (eq != std::string::npos) on = Tidy(CurrentValue(cond.substr(0, eq), "0")) == Tidy(cond.substr(eq + 1));
      const bool was = !e->HasAttribute("disabled");
      if (on != was) {
        if (on) e->RemoveAttribute("disabled");
        else e->SetAttribute("disabled", "");
      }
    }
  }
}

// ---------------------------------------------------------------- the game's controls

const char* KindName(mg::Kind k) {
  switch (k) {
    case mg::kButton: return "button";
    case mg::kCheck: return "checkbox";
    case mg::kRadio: return "radio button";
    case mg::kDropdown: return "dropdown";
    case mg::kEdit: return "edit box";
    case mg::kList: return "list";
    case mg::kSlider: return "slider";
    case mg::kScrollArrow: return "scroll arrow";
    case mg::kStatic: return "picture / text";
    default: return "?";
  }
}

std::string DialogScope(Rml::Element* e, const std::string& base) {
  for (Rml::Element* a = e; a; a = a->GetParentNode())
    if (a->HasAttribute("game-dialog")) return a->GetAttribute<Rml::String>("game-dialog", "");
  return base;
}

void BindGame(Rml::Element* e, const std::string& base_dtm) {
  GameBind b;
  b.el = e;
  b.only_if = !e->HasAttribute("game");
  b.name = e->GetAttribute<Rml::String>(b.only_if ? "game-if" : "game", "");
  b.scope_dtm = DialogScope(e, base_dtm);
  const Rml::String tag = e->GetTagName(), type = e->GetAttribute<Rml::String>("type", "");
  if (tag == "input") {
    b.ui = type == "checkbox" ? GameBind::kCheckInput : type == "range" ? GameBind::kRange : GameBind::kTextInput;
  } else if (tag == "select") {
    b.ui = GameBind::kSelect;
  } else if (e->IsClassSet("list")) {
    b.ui = GameBind::kList;
  } else if (!b.only_if && e->GetAttribute<Rml::String>("game-text", "on") != "off") {
    // The control's text goes into a child of class "t", else into the
    // element itself when it holds only text.
    bool only_text = true;
    for (int i = 0; i < e->GetNumChildren(); ++i) {
      Rml::Element* c = e->GetChild(i);
      if (c->IsClassSet("t")) {
        b.text_el = c;
        break;
      }
      if (c->GetTagName() != "#text") only_text = false;
    }
    if (!b.text_el && only_text) b.text_el = e;
  }
  for (int k = 0; k < 2; ++k) {
    std::vector<std::string>& v = k ? b.anim_out : b.anim_in;
    std::string urls = e->GetAttribute<Rml::String>(k ? "game-anim-out" : "game-anim-in", "");
    size_t at = 0;
    while (at < urls.size()) {
      size_t sp = urls.find(' ', at);
      if (sp == std::string::npos) sp = urls.size();
      if (sp > at) v.push_back(urls.substr(at, sp - at));
      at = sp + 1;
    }
  }
  b.window = e->HasAttribute("game-window");
  e->SetClass("game-hidden", true);  // until the control is seen
  g.game.push_back(b);
}

// A list's rows: <div class="row" row="N"> for the rows the game shows now
// (from its first row shown), .on the chosen one.
void SyncList(GameBind& b, const mg::Info& i) {
  std::vector<std::string> rows;
  mg::Rows(b.name.c_str(), &rows, b.scope);
  const int shown = i.visible > 0 ? i.visible : static_cast<int>(rows.size());
  if (rows == b.rows && i.top == b.list_top && i.value == b.list_value) return;
  if (rows != b.rows) patch::Log("MenuMaX: game list %s: %u rows, %d shown from %d", b.name.c_str(),
                                 static_cast<unsigned>(rows.size()), shown, i.top);
  b.rows = rows;
  b.list_top = i.top;
  b.list_value = i.value;
  // Rows are changed in place, not made again, so a row picked with the
  // keyboard / a controller keeps focus when the game's choice moves.
  const int first = i.top < 0 ? 0 : i.top;
  int n = 0;
  for (int r = first; r < static_cast<int>(rows.size()) && n < shown; ++r, ++n) {
    Rml::Element* row = n < b.el->GetNumChildren() ? b.el->GetChild(n) : nullptr;
    if (!row) {
      Rml::ElementPtr made = b.el->GetOwnerDocument()->CreateElement("div");
      row = b.el->AppendChild(std::move(made));
      row->SetClass("row", true);
    }
    row->SetClass("on", r == i.value);
    row->SetAttribute("row", r);
    if (row->GetInnerRML() != Rml::StringUtilities::EncodeRml(rows[r])) row->SetInnerRML(Rml::StringUtilities::EncodeRml(rows[r]));
  }
  while (b.el->GetNumChildren() > n) {
    Rml::Element* gone = b.el->GetChild(b.el->GetNumChildren() - 1);
    if (g.last_focus == gone) g.last_focus = nullptr;
    if (g.context && g.context->GetFocusElement() == gone) b.el->Focus();
    b.el->RemoveChild(gone);
  }
}

// A pop-out panel's animation: the game's frame, drawn as the element's
// picture (inline decorator over its own rule).
void SyncAnim(GameBind& b, const mg::Info& i) {
  const std::vector<std::string>& v = i.anim == 2 ? b.anim_out : b.anim_in;
  int want = 0;
  if ((i.anim == 1 || i.anim == 2) && !v.empty() && i.frame >= 0) {
    const int f = i.frame < static_cast<int>(v.size()) ? i.frame : static_cast<int>(v.size()) - 1;
    want = 1 + f + (i.anim == 2 ? 1000 : 0);
    if (want != b.anim_shown) b.el->SetProperty("decorator", "image(" + v[f] + ")");
  } else if (b.anim_shown > 0) {
    b.el->RemoveProperty("decorator");
  }
  // Every frame with MenuLog=1; otherwise when the animation starts or ends.
  if (want != b.anim_shown && (GetConfig().menu_log || (want == 0) != (b.anim_shown <= 0)))
    patch::Log("MenuMaX: game control %s: animation %d frame %d of %d (shown %d)", b.name.c_str(), i.anim, i.frame, i.frames,
               i.live ? 1 : 0);
  b.anim_shown = want;
}

void SyncOne(GameBind& b, DWORD now) {
  mg::Info i;
  const bool found = (b.scope || b.scope_dtm.empty()) && mg::Get(b.name.c_str(), &i, b.scope);
  // A panel with its animation's pictures shows while it animates;
  // anything else waits for the game's animation to end.
  const bool animated = !b.anim_in.empty() || !b.anim_out.empty();
  bool visible = found && (animated ? i.live : i.shown);
  if (animated) {
    const bool live = found && i.live;
    if (live && !b.was_live && b.seen && i.anim == 0) {
      b.anim_wait = 0;  // just shown, its animation not begun: wait for it
      patch::Log("MenuMaX: game control %s shown, waiting for its animation", b.name.c_str());
    }
    if (!live || i.anim != 0) b.anim_wait = -1;
    else if (b.anim_wait >= 0 && ++b.anim_wait > 12) {
      patch::Log("MenuMaX: game control %s: no animation came, shown as is", b.name.c_str());
      b.anim_wait = -1;
    }
    if (b.anim_wait >= 0) visible = false;
    // Showing, but the game has not reached its animation's first frame.
    if (live && i.anim == 1 && i.frame < 0) visible = false;
    if (i.anim == 1 && b.prev_anim != 1) {
      b.start_frame = i.frame;
      patch::Log("MenuMaX: game control %s: show animation begins at frame %d of %d", b.name.c_str(), i.frame, i.frames);
    }
    if (i.anim != 1) b.start_frame = -1;
    else if (b.start_frame > 0 && i.frame == b.start_frame) visible = false;
    else b.start_frame = -1;
    b.prev_anim = found ? i.anim : 0;
    b.was_live = live;
  }
  b.el->SetClass("game-hidden", !visible);
  if (found && animated) SyncAnim(b, i);
  if (!found) {
    if (b.seen) patch::Log("MenuMaX: game control %s gone", b.name.c_str());
    if (b.scope && !b.located) {  // its dialog is shown, the control is not on it
      b.located = true;
      mg::Locate(b.name.c_str());
    }
    b.seen = false;
    return;
  }
  const bool first = !b.seen;
  if (first)
    patch::Log("MenuMaX: game control %s: %s, %s%s%s, value %d of %d, text \"%s\" (%ld,%ld %ldx%ld)", b.name.c_str(),
               KindName(i.kind), i.shown ? "shown" : "hidden", i.enabled ? "" : ", greyed",
               i.tree ? "" : ", not marked shown in the GUI tree", i.value, i.range, i.text.c_str(),
               i.rect.left, i.rect.top, i.rect.right - i.rect.left, i.rect.bottom - i.rect.top);
  else if (i.shown != b.last.shown)
    patch::Log("MenuMaX: game control %s now %s", b.name.c_str(), i.shown ? "shown" : "hidden (or animating)");
  b.seen = true;
  if (b.only_if) {
    b.last = i;
    return;
  }
  b.el->SetClass("disabled", !i.enabled);
  if (b.ui != GameBind::kDiv) {
    if (i.enabled) b.el->RemoveAttribute("disabled");
    else if (!b.el->HasAttribute("disabled")) b.el->SetAttribute("disabled", "");
  }
  const bool settled = now - b.changed_at > 400 && !b.dragging;
  switch (b.ui) {
    case GameBind::kDiv:
      b.el->SetClass("on", (i.kind == mg::kCheck || i.kind == mg::kRadio) && i.value != 0);
      break;
    case GameBind::kCheckInput:
      if (!settled) break;
      if (i.value && !b.el->HasAttribute("checked")) b.el->SetAttribute("checked", "");
      else if (!i.value && b.el->HasAttribute("checked")) b.el->RemoveAttribute("checked");
      break;
    case GameBind::kRange: {
      auto* fc = dynamic_cast<Rml::ElementFormControl*>(b.el);
      if (!fc || !settled) break;
      if (i.range > 0 && b.el->GetAttribute<int>("max", -1) != i.range) {
        b.el->SetAttribute("min", 0);
        b.el->SetAttribute("max", i.range);
        b.el->SetAttribute("step", 1);
      }
      if (std::atoi(fc->GetValue().c_str()) != i.value) fc->SetValue(std::to_string(i.value));
      break;
    }
    case GameBind::kSelect: {
      auto* sel = dynamic_cast<Rml::ElementFormControlSelect*>(b.el);
      if (!sel) break;
      std::vector<std::string> rows;
      mg::Rows(b.name.c_str(), &rows, b.scope);
      if (rows != b.rows) {
        b.rows = rows;
        sel->RemoveAll();
        for (size_t r = 0; r < rows.size(); ++r) sel->Add(Rml::StringUtilities::EncodeRml(rows[r]), std::to_string(r));
        patch::Log("MenuMaX: game list %s: %u rows", b.name.c_str(), static_cast<unsigned>(rows.size()));
      }
      if (settled && !sel->IsSelectBoxVisible() && sel->GetSelection() != i.value) sel->SetSelection(i.value);
      break;
    }
    case GameBind::kTextInput: {
      auto* fc = dynamic_cast<Rml::ElementFormControl*>(b.el);
      if (fc && settled && g.context->GetFocusElement() != b.el && fc->GetValue() != i.text) fc->SetValue(i.text);
      break;
    }
    case GameBind::kList:
      SyncList(b, i);
      break;
  }
  if (b.text_el && !i.text.empty() && (first || i.text != b.last.text)) b.text_el->SetInnerRML(Rml::StringUtilities::EncodeRml(i.text));
  b.last = i;
}

uintptr_t DialogNamed(const std::string& dtm) {
  if (dtm.empty()) return 0;
  // The newest one shown with that layout.
  for (auto it = g.dialogs.rbegin(); it != g.dialogs.rend(); ++it)
    if (it->dtm == dtm) return it->p;
  return 0;
}

// Every frame a screen tied to the game's controls is open: shows their
// state.
void SyncGame() {
  if (g.game.empty() && g.boxes.empty()) return;
  const DWORD now = GetTickCount();
  if (!g.mirror) {  // (MaXMenus does this itself)
    mg::Refresh();
    mg::Dialogs(&g.dialogs);
  }
  for (DialogBox& d : g.boxes) {
    const mg::Dialog* dlg = nullptr;
    for (auto it = g.dialogs.rbegin(); it != g.dialogs.rend() && !dlg; ++it)
      if (it->dtm == d.dtm) dlg = &*it;
    d.el->SetClass("game-hidden", !dlg);
    if (!dlg) continue;
    const long x = dlg->rect.left, y = dlg->rect.top;
    if ((x != d.left || y != d.top) && x > -640 && x < 640 && y > -480 && y < 480) {
      d.left = x;
      d.top = y;
      d.el->SetProperty("left", std::to_string(x) + "dp");
      d.el->SetProperty("top", std::to_string(y) + "dp");
      patch::Log("MenuMaX: %s placed at %ld,%ld as the game's", d.dtm.c_str(), x, y);
    }
  }
  g.updating = true;
  for (GameBind& b : g.game) {
    b.scope = DialogNamed(b.scope_dtm);
    SyncOne(b, now);
    // A slider let go: the release the game gets at the end of a drag.
    if (b.dragging && now - b.changed_at > 300) {
      b.dragging = false;
      if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(b.el))
        mg::SetSlider(b.name.c_str(), std::atoi(fc->GetValue().c_str()), 16, b.scope);
    }
  }
  g.updating = false;
}

// A click on an element tied to a button, checkbox, radio button or tab.
void OnGameClick(Rml::Element* target) {
  int row = -1;
  for (Rml::Element* e = target; e; e = e->GetParentNode()) {
    if (row < 0 && e->HasAttribute("row")) row = e->GetAttribute<int>("row", -1);
    for (GameBind& b : g.game) {
      if (b.el != e || b.only_if) continue;
      if (b.ui == GameBind::kList) {
        if (row >= 0 && b.seen && b.last.enabled) {
          ++g.clicks;
          mg::Choose(b.name.c_str(), row, b.scope);
        }
        return;
      }
      if (b.ui != GameBind::kDiv) continue;
      if (!b.seen || !b.last.enabled || !b.last.shown) {
        patch::Log("MenuMaX: %s clicked, but the game's control is %s", b.name.c_str(),
                   !b.seen ? "not there" : !b.last.enabled ? "greyed out" : "hidden");
        return;
      }
      const mg::Kind k = b.last.kind;
      if (k == mg::kButton || k == mg::kCheck || k == mg::kRadio || k == mg::kScrollArrow) {
        ++g.clicks;
        mg::Click(b.name.c_str(), b.scope);
      }
      return;
    }
  }
}

std::vector<Rml::ElementDocument*> Docs() {
  std::vector<Rml::ElementDocument*> d;
  if (g.mirror) {
    for (const Layer& l : g.layers)
      if (l.doc) d.push_back(l.doc);
  } else if (g.doc) {
    d.push_back(g.doc);
  }
  return d;
}

std::vector<std::string> Words(const std::string& c) {
  std::vector<std::string> out;
  for (size_t a = 0; a < c.size();) {
    size_t z = c.find(' ', a);
    if (z == std::string::npos) z = c.size();
    if (z > a) out.push_back(c.substr(a, z - a));
    a = z + 1;
  }
  return out;
}

void Bind() {
  g.settings.clear();
  g.game.clear();
  g.boxes.clear();
  g.binding = true;
  for (Rml::ElementDocument* doc : Docs()) {
    const std::vector<std::string> dialogs = Words(doc->GetAttribute<Rml::String>("game-dialogs", ""));
    const std::string base = dialogs.empty() ? std::string() : dialogs[0];
    std::vector<Rml::Element*> els;
    Collect(doc, els);
    for (Rml::Element* e : els) {
      if (e->HasAttribute("game-dialog")) {
        DialogBox d;
        d.el = e;
        d.dtm = e->GetAttribute<Rml::String>("game-dialog", "");
        e->SetClass("game-hidden", true);
        g.boxes.push_back(d);
      }
      if (e->HasAttribute("game") || e->HasAttribute("game-if")) {
        BindGame(e, base);
        continue;
      }
      if (!e->HasAttribute("ini")) continue;
      Setting s;
      s.el = e;
      s.key = e->GetAttribute<Rml::String>("ini", "");
      s.checkbox = e->GetTagName() == "input" && e->GetAttribute<Rml::String>("type", "") == "checkbox";
      const std::string v = CurrentValue(s.key, e->GetAttribute<Rml::String>("default", "0"));
      s.on_value = !IsZero(v) && s.checkbox ? v : e->GetAttribute<Rml::String>("ini-on", "1");
      SetControl(s, v);
      patch::Log("MenuMaX: setting %s = %s (%s%s)", s.key.c_str(), v.c_str(), e->GetTagName().c_str(), s.checkbox ? " checkbox" : "");
      g.settings.push_back(s);
    }
  }
  g.binding = false;
  // The top screen: game-back="Back" (Esc / B / the right mouse button
  // clicks it, on the screen's first game dialog).
  g.back_control.clear();
  g.back_dtm.clear();
  if (g.doc) {
    g.back_control = g.doc->GetAttribute<Rml::String>("game-back", "");
    const std::vector<std::string> dialogs = Words(g.doc->GetAttribute<Rml::String>("game-dialogs", ""));
    if (!dialogs.empty()) g.back_dtm = dialogs[0];
  }
  UpdateTexts();
  patch::Log("MenuMaX: %u settings, %u game controls and %u game dialogs bound on %s%s%s", static_cast<unsigned>(g.settings.size()),
             static_cast<unsigned>(g.game.size()), static_cast<unsigned>(g.boxes.size()), g.open_name.c_str(),
             g.back_control.empty() ? "" : "; back = ", g.back_control.c_str());
}

void Save(const std::string& key, const std::string& v) {
  const char* file;
  char* end = nullptr;
  const long iv = std::strtol(v.c_str(), &end, 10);
  // Whole numbers through SetFromMenu: the settings that can change at once
  // (VSync, MotionBlur, AlphaToCoverage, Anisotropy) do.
  if (!v.empty() && end && !*end && key != "ViewDistance") file = SetFromMenu(key.c_str(), static_cast<int>(iv));
  else file = SetStringFromMenu(key.c_str(), v.c_str());
  ++g.saves;
  patch::Log("MenuMaX: %s=%s saved to %s", key.c_str(), v.c_str(), file);
}

void FlushPending(const char* why) {
  if (g.pending.empty()) return;
  patch::Log("MenuMaX: saving %u setting(s) (%s)", static_cast<unsigned>(g.pending.size()), why);
  for (const auto& kv : g.pending) Save(kv.first, kv.second);
  g.pending.clear();
}

void OnChange(Rml::Element* target) {
  if (g.binding || g.updating) return;
  for (GameBind& b : g.game) {
    if (b.el != target || b.only_if) continue;
    const DWORD now = GetTickCount();
    auto* fc = dynamic_cast<Rml::ElementFormControl*>(target);
    switch (b.ui) {
      case GameBind::kCheckInput:
        mg::Click(b.name.c_str(), b.scope);
        break;
      case GameBind::kRange: {
        const int v = fc ? std::atoi(fc->GetValue().c_str()) : 0;
        if (!b.dragging) mg::SetSlider(b.name.c_str(), v, 15, b.scope);  // grab
        mg::SetSlider(b.name.c_str(), v, 3, b.scope);                   // change
        b.dragging = true;
        break;
      }
      case GameBind::kSelect:
        if (fc && !fc->GetValue().empty()) mg::Choose(b.name.c_str(), std::atoi(fc->GetValue().c_str()), b.scope);
        break;
      case GameBind::kTextInput:
        if (fc) mg::SetText(b.name.c_str(), fc->GetValue(), b.scope);
        break;
      default:
        break;
    }
    b.changed_at = now;
    if (target->HasAttribute("restart") && g.doc) g.doc->SetClass("restart-needed", true);
    return;
  }
  for (Setting& s : g.settings) {
    if (s.el != target) continue;
    std::string v;
    if (s.checkbox) v = target->HasAttribute("checked") ? s.on_value : "0";
    else if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(target)) v = Tidy(fc->GetValue());
    if (v == s.value) return;
    patch::Log("MenuMaX: setting %s: %s -> %s", s.key.c_str(), s.value.c_str(), v.c_str());
    s.value = v;
    if (s.checkbox && !IsZero(v)) s.on_value = v;
    g.pending[s.key] = v;  // saved once it stops changing (sliders send many)
    g.last_change = GetTickCount();
    if (target->HasAttribute("restart") && g.doc) g.doc->SetClass("restart-needed", true);
    UpdateTexts();
    return;
  }
}

// ---------------------------------------------------------------- events

class ChangeListener : public Rml::EventListener {
 public:
  void ProcessEvent(Rml::Event& e) override { OnChange(e.GetTargetElement()); }
};
ChangeListener g_change;

class ClickListener : public Rml::EventListener {
 public:
  void ProcessEvent(Rml::Event& e) override { OnGameClick(e.GetTargetElement()); }
};
ClickListener g_click;

// onclick="close" / "open <screen>" / "reload": what a button does.
class Command : public Rml::EventListener {
 public:
  explicit Command(const Rml::String& c) : cmd_(c) {}
  void ProcessEvent(Rml::Event& e) override {
    ++g.clicks;
    patch::Log("MenuMaX: %s on %s#%s: %s", e.GetType().c_str(), e.GetCurrentElement()->GetTagName().c_str(),
               e.GetCurrentElement()->GetId().c_str(), cmd_.c_str());
    if (cmd_ == "close" || cmd_ == "back") {
      Close("button");
    } else if (cmd_.compare(0, 5, "open ") == 0) {
      Open(cmd_.substr(5).c_str(), "button");
    } else if (cmd_ == "reload") {
      g.signature = 0;  // reloads at the next look
      g.last_watch = 0;
    } else {
      patch::Log("MenuMaX: unknown command \"%s\" (close, back, open <screen>, reload)", cmd_.c_str());
    }
  }
  void OnDetach(Rml::Element*) override { delete this; }

 private:
  std::string cmd_;
};
class Commands : public Rml::EventListenerInstancer {
 public:
  Rml::EventListener* InstanceEventListener(const Rml::String& value, Rml::Element*) override { return new Command(value); }
};
Commands g_commands;

// ---------------------------------------------------------------- files

void Signature(const std::wstring& dir, unsigned long long& sig, int depth) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.cFileName[0] == L'.') continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth < 4) Signature(dir + fd.cFileName + L"\\", sig, depth + 1);
      continue;
    }
    unsigned long long t = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                           fd.ftLastWriteTime.dwLowDateTime;
    sig = sig * 1099511628211ull ^ (t + fd.nFileSizeLow);
    for (const wchar_t* c = fd.cFileName; *c; ++c) sig = sig * 31 + *c;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void LoadFonts() {
  int n = 0;
  auto load = [&](const std::wstring& path, bool fallback) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const bool ok = Rml::LoadFontFace(Narrow(path), fallback);
    patch::Log("MenuMaX: font %ls %s%s", path.c_str(), ok ? "loaded" : "FAILED", fallback ? " (fallback)" : "");
    n += ok;
  };
  // UI\MaX\fonts\ first (the screens' own fonts), then the HUD fonts, then
  // Windows' own (on every Windows since XP) so text always shows.
  for (const wchar_t* pat : {L"*.ttf", L"*.otf"}) {
    WIN32_FIND_DATAW fd;
    const std::wstring dir = UiDir() + L"fonts\\";
    HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do load(dir + fd.cFileName, false);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  load(GameDir() + L"FONTS\\ContrailOne-Regular.ttf", false);
  wchar_t win[MAX_PATH] = {};
  GetWindowsDirectoryW(win, MAX_PATH);
  const std::wstring fonts = std::wstring(win) + L"\\Fonts\\";
  load(fonts + L"arial.ttf", true);
  load(fonts + L"arialbd.ttf", false);
  load(fonts + L"tahoma.ttf", false);
  load(fonts + L"tahomabd.ttf", false);
  patch::Log("MenuMaX: %d font faces", n);
}

bool InitRml() {
  if (g.inited) return true;
  if (g.failed) return false;
  const DWORD t0 = GetTickCount();
  g.system = new System;
  g.files = new Files;
  if (dd::IsD3D9()) {
    g.r9 = new RenderD3D9;
    g.render = g.r9;
    patch::Log("MenuMaX: drawing with Direct3D 9 (fixed function)");
  } else {
    g.r11 = new RenderD3D11;
    g.render = g.r11;
    if (!g.r11->Init(dd::B().device)) {
      g.failed = true;
      return false;
    }
  }
  g.render->SetBaseDir(UiDir());
  Rml::SetSystemInterface(g.system);
  Rml::SetFileInterface(g.files);
  Rml::SetRenderInterface(g.render);
  if (!Rml::Initialise()) {
    patch::Log("MenuMaX: RmlUi could not start");
    g.failed = true;
    return false;
  }
  Rml::Factory::RegisterEventListenerInstancer(&g_commands);
  LoadFonts();
  g.context = Rml::CreateContext("menu", Rml::Vector2i(640, 480));
  if (!g.context) {
    patch::Log("MenuMaX: RmlUi context could not be made");
    g.failed = true;
    return false;
  }
  g.inited = true;
  patch::Log("MenuMaX: RmlUi %s started in %lu ms; screens from %ls", Rml::GetVersion().c_str(),
             static_cast<unsigned long>(GetTickCount() - t0), UiDir().c_str());
  return true;
}

void CloseMirror(const char* why);

void CloseDoc() {
  if (g.mirror) {
    CloseMirror("another screen opens");
    return;
  }
  if (!g.doc) return;
  FlushPending("screen closed");
  g.doc->RemoveEventListener(Rml::EventId::Change, &g_change);
  g.doc->RemoveEventListener(Rml::EventId::Click, &g_click);
  g.doc->Close();
  g.doc = nullptr;
  g.last_focus = nullptr;
  g.settings.clear();
  g.game.clear();
  g.boxes.clear();
  g.back_control.clear();
  g.context->Update();  // documents go away at the next update
}

// ---------------------------------------------------------------- MaXMenus

std::wstring MenusDir() { return UiDir() + L"menus\\"; }

// UI\MaX\menus\*.rml: which game dialogs each copies (game-dialogs= on its
// <body>), and whether it covers the whole menu (game-opaque).
void ReadIndex() {
  g.index.clear();
  g.index_info.clear();
  g.index_read = true;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((MenusDir() + L"*.rml").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    patch::Log("MenuMaX: no screens in %ls; the game's own menus are shown", MenusDir().c_str());
    return;
  }
  do {
    const std::wstring path = MenusDir() + fd.cFileName;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) continue;
    std::string text(8192, '\0');
    text.resize(std::fread(&text[0], 1, text.size(), f));
    std::fclose(f);
    const size_t body = text.find("<body");
    if (body == std::string::npos) continue;
    const std::string tag = text.substr(body, text.find('>', body) - body);
    const size_t at = tag.find("game-dialogs=\"");
    if (at == std::string::npos) continue;
    const size_t end = tag.find('"', at + 14);
    std::vector<std::string> dialogs = Words(tag.substr(at + 14, end - at - 14));
    for (std::string& d : dialogs)
      for (char& c : d)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    std::string file = Narrow(fd.cFileName);
    file.resize(file.size() - 4);
    const bool opaque = tag.find("game-opaque") != std::string::npos;
    g.index_info[file] = {opaque, dialogs};
    for (const std::string& d : dialogs) {
      if (g.index.count(d)) patch::Log("MenuMaX: game dialog %s is in both %s and %s; %s used", d.c_str(), g.index[d].c_str(),
                                       file.c_str(), g.index[d].c_str());
      else g.index[d] = file;
    }
    std::string list;
    for (const std::string& d : dialogs) list += " " + d;
    patch::Log("MenuMaX: menus\\%s.rml copies%s%s", file.c_str(), list.c_str(), opaque ? " (covers the whole menu)" : "");
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void CloseMirror(const char* why) {
  if (!g.mirror) return;
  patch::Log("MenuMaX: game menu copies closed (%s)", why);
  for (Layer& l : g.layers) {
    if (!l.doc) continue;
    l.doc->RemoveEventListener(Rml::EventId::Change, &g_change);
    l.doc->RemoveEventListener(Rml::EventId::Click, &g_click);
    l.doc->Close();
  }
  g.layers.clear();
  g.mirror = false;
  g.doc = nullptr;
  g.last_focus = nullptr;
  g.game.clear();
  g.boxes.clear();
  g.settings.clear();
  g.back_control.clear();
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    g.active = false;
    g.held = Rml::Input::KI_UNKNOWN;
    g.pad_dir_prev = -1;
  }
  g.context->Update();
}

// Which screens the game's dialogs need now, bottom first. From the top
// dialog down: each needs a screen, until one that covers the whole menu.
// Empty when one is not copied (the game's own menus are shown).
std::vector<std::string> Wanted() {
  std::vector<std::string> top_down;
  bool all = true;
  std::string missing;
  for (auto it = g.dialogs.rbegin(); it != g.dialogs.rend(); ++it) {
    if (it->names.empty()) continue;
    auto f = g.index.find(it->dtm);
    if (f == g.index.end()) {
      all = false;
      missing = it->dtm;
      break;
    }
    if (std::find(top_down.begin(), top_down.end(), f->second) == top_down.end()) top_down.push_back(f->second);
    if (g.index_info[f->second].first) break;
  }
  if (!all || top_down.empty()) {
    if (missing != g.missing && !missing.empty())
      patch::Log("MenuMaX: the game's %s dialog has no copy in menus\\; the game's own menus are shown", missing.c_str());
    g.missing = missing;
    return {};
  }
  g.missing.clear();
  return std::vector<std::string>(top_down.rbegin(), top_down.rend());
}

// MaXMenus, every frame: the screens follow the game's dialogs.
void Mirror() {
  if (!GetConfig().max_menus || (g.doc && !g.mirror) || !g.want_open.empty()) return;
  if (!g.index_read) ReadIndex();
  mg::Refresh();
  mg::Dialogs(&g.dialogs);
  bool capture = false;
  for (const mg::Dialog& d : g.dialogs)
    if (d.dtm == "optcont" && mg::DialogField(d.p, 0x7F70)) capture = true;
  if (capture != g.capture) {
    g.capture = capture;
    patch::Log("MenuMaX: %s", capture ? "the game waits for a key or button to assign: input goes to the game"
                                      : "the game has its key or button: input comes here again");
    std::lock_guard<std::recursive_mutex> l(g.mu);
    g.keys.clear();
    g.chars.clear();
    g.buttons.clear();
    g.opened_at = GetTickCount();
  }
  const std::vector<std::string> want = Wanted();
  std::vector<std::string> have;
  for (const Layer& l : g.layers) have.push_back(l.file);
  if (want == have) return;
  if (want.empty()) {
    CloseMirror("a game dialog shown is not copied, or no menu is shown");
    MenusRefreshFromIni();
    return;
  }
  if (!InitRml()) return;
  const bool first = !g.mirror;
  const std::string old_top = have.empty() ? std::string() : have.back();
  std::string list;
  for (const std::string& w : want) list += " " + w;
  patch::Log("MenuMaX: game menu copies shown:%s", list.c_str());
  // Screens no longer wanted go; new ones load.
  std::vector<Layer> next;
  for (const std::string& w : want) {
    Layer l;
    for (Layer& o : g.layers)
      if (o.file == w && o.doc) {
        l = o;
        o.doc = nullptr;
      }
    if (!l.doc) {
      const DWORD t0 = GetTickCount();
      l.file = w;
      l.doc = g.context->LoadDocument(Narrow(MenusDir()) + w + ".rml");
      if (!l.doc) {
        patch::Log("MenuMaX: menus\\%s.rml could not be loaded (see the RmlUi messages above); the game's own menus are shown",
                   w.c_str());
        for (Layer& n : next) {
          n.doc->RemoveEventListener(Rml::EventId::Change, &g_change);
          n.doc->RemoveEventListener(Rml::EventId::Click, &g_click);
          n.doc->Close();
        }
        CloseMirror("a screen could not be loaded");
        // Not tried again until the files change.
        for (const std::string& d : g.index_info[w].second) g.index.erase(d);
        return;
      }
      l.doc->AddEventListener(Rml::EventId::Change, &g_change);
      l.doc->AddEventListener(Rml::EventId::Click, &g_click);
      l.opaque = g.index_info[w].first;
      l.dialogs = g.index_info[w].second;
      patch::Log("MenuMaX: menus\\%s.rml loaded in %lu ms", w.c_str(), static_cast<unsigned long>(GetTickCount() - t0));
      g.dump_at = GetTickCount() + 1500;
    }
    next.push_back(l);
  }
  for (Layer& o : g.layers) {
    if (!o.doc) continue;
    o.doc->RemoveEventListener(Rml::EventId::Change, &g_change);
    o.doc->RemoveEventListener(Rml::EventId::Click, &g_click);
    o.doc->Close();
  }
  g.layers = next;
  g.mirror = true;
  g.open_name = "menus";
  for (size_t i = 0; i < g.layers.size(); ++i) {
    Rml::ElementDocument* d = g.layers[i].doc;
    const bool top = i + 1 == g.layers.size();
    d->Show(top ? Rml::ModalFlag::Modal : Rml::ModalFlag::None, top ? Rml::FocusFlag::Auto : Rml::FocusFlag::None);
    d->PullToFront();
  }
  g.doc = g.layers.back().doc;
  if (g.layers.back().file != old_top) {
    g.last_focus = nullptr;
    std::lock_guard<std::recursive_mutex> l(g.mu);
    g.keys.clear();
    g.buttons.clear();
    g.held = Rml::Input::KI_UNKNOWN;
    g.pad_dir_prev = -1;
    g.opened_at = GetTickCount();  // a key that opened it is not a press on it
  }
  Bind();
  if (first) {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    g.active = true;
    g.pointer_set = false;
    g.last_report = GetTickCount();
    std::memset(g.key_down, 0, sizeof g.key_down);
    g.last_key = Rml::Input::KI_UNKNOWN;
    g.last_key_source = -1;
    g.signature = 0;
    Signature(UiDir(), g.signature, 0);
    g.last_watch = GetTickCount();
  }
}

bool LoadDoc(const std::string& name, const std::string& focus_id) {
  CloseDoc();
  const DWORD t0 = GetTickCount();
  const std::string path = Narrow(UiDir()) + name + ".rml";
  g.doc = g.context->LoadDocument(path);
  if (!g.doc) {
    patch::Log("MenuMaX: %s could not be loaded (see the RmlUi messages above)", path.c_str());
    return false;
  }
  g.doc->AddEventListener(Rml::EventId::Change, &g_change);
  g.doc->AddEventListener(Rml::EventId::Click, &g_click);
  g.open_name = name;
  Bind();
  g.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
  Rml::Element* f = focus_id.empty() ? nullptr : g.doc->GetElementById(focus_id);
  if (!f) {
    Rml::ElementList list;
    g.doc->QuerySelectorAll(list, "[autofocus]");
    if (!list.empty()) f = list[0];
  }
  if (f) f->Focus(true);
  g.signature = 0;
  Signature(UiDir(), g.signature, 0);
  g.last_watch = GetTickCount();
  patch::Log("MenuMaX: %s loaded in %lu ms (focus on %s)", path.c_str(), static_cast<unsigned long>(GetTickCount() - t0),
             f ? f->GetId().c_str() : "nothing");
  return true;
}

void Watch() {
  const DWORD now = GetTickCount();
  if (now - g.last_watch < 500) return;
  g.last_watch = now;
  unsigned long long sig = 0;
  Signature(UiDir(), sig, 0);
  if (sig == g.signature) return;
  const std::string focus = g.context->GetFocusElement() ? g.context->GetFocusElement()->GetId() : std::string();
  patch::Log("MenuMaX: files in UI\\MaX changed; reloading %s", g.open_name.c_str());
  ++g.reloads;
  FlushPending("reload");
  Rml::Factory::ClearStyleSheetCache();
  Rml::Factory::ClearTemplateCache();
  if (g.mirror) {
    // Read again at the next frame.
    CloseMirror("files changed");
    Rml::ReleaseTextures();
    g.index_read = false;
    g.signature = sig;
    return;
  }
  CloseDoc();
  Rml::ReleaseTextures();
  if (!LoadDoc(g.open_name, focus)) {
    g.signature = sig;  // wait for the next change
    patch::Log("MenuMaX: the screen stays closed until its files load again (Esc / B leaves)");
  }
}

// ---------------------------------------------------------------- input

Rml::Input::KeyIdentifier FromDik(unsigned dik) {
  using namespace Rml::Input;
  switch (dik) {
    case 0x01: return KI_ESCAPE;
    case 0x0E: return KI_BACK;
    case 0x0F: return KI_TAB;
    case 0x1C: return KI_RETURN;
    case 0x9C: return KI_NUMPADENTER;
    case 0x39: return KI_SPACE;
    case 0xC8: case 0x48: return KI_UP;
    case 0xD0: case 0x50: return KI_DOWN;
    case 0xCB: case 0x4B: return KI_LEFT;
    case 0xCD: case 0x4D: return KI_RIGHT;
    case 0xC9: return KI_PRIOR;
    case 0xD1: return KI_NEXT;
    case 0xC7: return KI_HOME;
    case 0xCF: return KI_END;
    case 0xD3: return KI_DELETE;
    case 0x3F: return KI_F5;
    case 0x2A: return KI_LSHIFT;
    case 0x36: return KI_RSHIFT;
    default: return KI_UNKNOWN;
  }
}

bool Repeats(Rml::Input::KeyIdentifier k) {
  using namespace Rml::Input;
  return k == KI_UP || k == KI_DOWN || k == KI_LEFT || k == KI_RIGHT || k == KI_TAB;
}

bool IsControl(Rml::Element* e) {
  if (!e) return false;
  const Rml::String& t = e->GetTagName();
  return t == "input" || t == "select" || t == "button" || e->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto;
}

// Keyboard / controller moves: the pointer goes onto the control picked
// (a checkbox's or radio button's box, else the middle), so it lights up as
// under the mouse, as MenuNav does on the game's own screens.
void PointAt(Rml::Element* e) {
  if (!e || !g.context) return;
  const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
  const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
  const float dp = g.context->GetDensityIndependentPixelRatio();
  float x = at.x + size.x * 0.5f, y = at.y + size.y * 0.5f;
  if (e->IsClassSet("check") || e->IsClassSet("radio")) x = at.x + 7.0f * dp;
  g.px = x;
  g.py = y;
  g.pointer_set = true;
  g.mouse_x = static_cast<int>(x);
  g.mouse_y = static_cast<int>(y);
  g.context->ProcessMouseMove(g.mouse_x, g.mouse_y, 0);
}

bool TakesArrows(Rml::Element* e) {
  const Rml::String& t = e->GetTagName();
  return t == "input" || t == "select" || t == "textarea";
}

// nav-up / nav-down / ... set to something other than auto on the control.
bool NavSet(Rml::Element* e, Rml::Input::KeyIdentifier k) {
  using namespace Rml::Input;
  const Rml::PropertyId id = k == KI_UP ? Rml::PropertyId::NavUp : k == KI_DOWN ? Rml::PropertyId::NavDown
                           : k == KI_LEFT ? Rml::PropertyId::NavLeft : Rml::PropertyId::NavRight;
  const Rml::Property* p = e->GetProperty(id);
  return p && p->unit != Rml::Unit::KEYWORD;
}

// Arrow keys / d-pad: the nearest control whose middle lies that way
// (distance that way + twice the distance across). RmlUi's own pick skips
// controls that overlap the current one by even a pixel (Replay and Quit on
// the main menu) and can jump past near ones.
bool NavMove(Rml::Element* from, Rml::Input::KeyIdentifier k) {
  using namespace Rml::Input;
  Rml::ElementDocument* doc = from->GetOwnerDocument();
  if (!doc) return false;
  auto middle = [](Rml::Element* e) {
    const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    return Rml::Vector2f(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
  };
  const Rml::Vector2f m0 = middle(from);
  Rml::ElementList list;
  doc->QuerySelectorAll(list, "input, select, button, .tab, .button, .check, .radio, .pick, .cycle, .slider, .dropdown, .edit, .list .row");
  Rml::Element* best = nullptr;
  float best_score = 0;
  for (Rml::Element* e : list) {
    if (e == from || !e->IsVisible(true) || !IsControl(e) || e->IsClassSet("disabled") || e->IsClassSet("game-hidden")) continue;
    const Rml::Vector2f m = middle(e);
    float along = 0, across = 0;
    switch (k) {
      case KI_DOWN: along = m.y - m0.y; across = m.x - m0.x; break;
      case KI_UP: along = m0.y - m.y; across = m.x - m0.x; break;
      case KI_RIGHT: along = m.x - m0.x; across = m.y - m0.y; break;
      case KI_LEFT: along = m0.x - m.x; across = m.y - m0.y; break;
      default: return false;
    }
    if (along <= 1.0f) continue;
    const float score = along + 2.0f * std::fabs(across);
    if (!best || score < best_score) {
      best = e;
      best_score = score;
    }
  }
  if (!best || !best->Focus(true)) return false;
  patch::Log("MenuMaX: key %d: %s#%s -> %s#%s", static_cast<int>(k), from->GetTagName().c_str(), from->GetId().c_str(),
               best->GetTagName().c_str(), best->GetId().c_str());
  return true;
}

// After any input: remember the focused control, or give focus back to the
// last one when a click on empty space or text took it away.
void KeepFocus(const char* why) {
  if (!g.context || !g.doc) return;
  Rml::Element* f = g.context->GetFocusElement();
  if (IsControl(f)) {
    g.last_focus = f;
    return;
  }
  if (g.last_focus) {
    g.last_focus->Focus();
    if (GetConfig().menu_log) patch::Log("MenuMaX: focus kept on %s#%s (%s)", g.last_focus->GetTagName().c_str(),
                                         g.last_focus->GetId().c_str(), why);
  }
}

// Where a press came from: 0 keyboard, 1 controller, 2 other.
int SourceOf(const char* from) {
  if (std::strcmp(from, "keyboard") == 0) return 0;
  if (std::strncmp(from, "controller", 10) == 0 || std::strncmp(from, "joystick", 8) == 0) return 1;
  return 2;
}

void Press(Rml::Input::KeyIdentifier k, const char* from) {
  using namespace Rml::Input;
  {
    const int src = SourceOf(from);
    const DWORD now = GetTickCount();
    if (src < 2 && k == g.last_key && g.last_key_source >= 0 && g.last_key_source != src && now - g.last_key_at < 150) {
      ++g.dup_keys;
      if (GetConfig().menu_log || g.dup_keys <= 10)
        patch::Log("MenuMaX: key %d from %s ignored: the same press came from the %s %lu ms before", static_cast<int>(k),
                   from, g.last_key_source == 0 ? "keyboard" : "controller", static_cast<unsigned long>(now - g.last_key_at));
      g.last_key_at = now;
      return;
    }
    if (src < 2) {
      g.last_key = k;
      g.last_key_source = src;
      g.last_key_at = now;
    }
  }
  ++g.key_events;
  auto* select = g.context ? dynamic_cast<Rml::ElementFormControlSelect*>(g.context->GetFocusElement()) : nullptr;
  if (select && select->IsSelectBoxVisible() && (k == KI_ESCAPE || k == KI_BACK)) {
    select->CancelSelectBox();  // Esc / B closes an open list first
    return;
  }
  if (select && select->IsSelectBoxVisible() && (k == KI_UP || k == KI_DOWN)) {
    // An open list: Up / Down move through its rows (Enter / A picks), and
    // never on to another control.
    g.context->ProcessKeyDown(k, g.shift ? KM_SHIFT : 0);
    g.context->ProcessKeyUp(k, g.shift ? KM_SHIFT : 0);
    if (GetConfig().menu_log) patch::Log("MenuMaX: key %d (%s) in the open list #%s", static_cast<int>(k), from, select->GetId().c_str());
    return;
  }
  if (select && !select->IsSelectBoxVisible() && (k == KI_UP || k == KI_DOWN)) {
    // A closed list: Up / Down go to the next control only (RmlUi would also
    // change the list's choice).
    if (!NavSet(select, k)) NavMove(select, k);
    else {
      g.context->ProcessKeyDown(k, g.shift ? KM_SHIFT : 0);
      g.context->ProcessKeyUp(k, g.shift ? KM_SHIFT : 0);
    }
    if (g.doc) KeepFocus("key");
    if (g.doc && g.context->GetFocusElement() != select && IsControl(g.context->GetFocusElement()))
      PointAt(g.context->GetFocusElement());
    return;
  }
  if (select && !select->IsSelectBoxVisible() && (k == KI_LEFT || k == KI_RIGHT)) {
    // Left / Right step through a closed list's choices, like a slider.
    const int n = select->GetNumOptions(), i = select->GetSelection() + (k == KI_RIGHT ? 1 : -1);
    if (i >= 0 && i < n) select->SetSelection(i);
    return;
  }
  if (k == KI_ESCAPE) {
    if (!g.back_control.empty()) {
      patch::Log("MenuMaX: back (%s): the game's %s%s%s", from, g.back_control.c_str(), g.back_dtm.empty() ? "" : " on ",
                 g.back_dtm.c_str());
      mg::Click(g.back_control.c_str(), DialogNamed(g.back_dtm));
      return;
    }
    // A message box (messbox1/2/3, e.g. the pause box in races) has no fixed
    // back button: its buttons' texts change with use. Esc clicks the shown
    // button reading Cancel, else Resume / Continue / Back / No, else the
    // only button shown.
    {
      static const char* const kWords[] = {"cancel", "resume", "continue", "back", "no"};
      const GameBind* pick = nullptr;
      int pick_rank = 99, buttons = 0;
      const GameBind* only = nullptr;
      for (const GameBind& b : g.game) {
        if (b.ui != GameBind::kDiv || !b.el || !b.el->IsClassSet("button") || !b.scope) continue;
        if (!b.last.shown || !b.last.enabled) continue;
        ++buttons;
        only = &b;
        std::string t;
        for (char c : b.last.text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!t.empty() && (t.back() == ' ' || t.back() == '.')) t.pop_back();
        for (int i = 0; i < static_cast<int>(sizeof kWords / sizeof kWords[0]); ++i)
          if (t == kWords[i] && i < pick_rank) {
            pick = &b;
            pick_rank = i;
          }
      }
      if (!pick && buttons == 1) pick = only;
      if (pick) {
        patch::Log("MenuMaX: back (%s): the game's %s (\"%s\", of %d buttons shown)", from, pick->name.c_str(),
                   pick->last.text.c_str(), buttons);
        mg::Click(pick->name.c_str(), pick->scope);
        return;
      }
      if (buttons) patch::Log("MenuMaX: back (%s): no Cancel / No button among %d shown", from, buttons);
    }
    patch::Log("MenuMaX: back (%s)", from);
    if (!g.mirror) Close(from);
    return;
  }
  if (k == KI_F5) {
    g.signature = 0;
    g.last_watch = 0;
    patch::Log("MenuMaX: F5: reload");
    return;
  }
  if (!g.context) return;
  if (!IsControl(g.context->GetFocusElement())) {
    if (g.last_focus) {
      g.last_focus->Focus(true);
    } else if (Repeats(k) && g.doc) {
      // Nothing picked yet: the first move picks the lit tab (.tab.on), else
      // the first control shown.
      Rml::ElementList list;
      g.doc->QuerySelectorAll(list, ".tab.on");
      if (list.empty()) g.doc->QuerySelectorAll(list, "input, select, button, .tab, .button, .check, .radio, .cycle");
      for (Rml::Element* e : list) {
        if (!e->IsVisible(true) || !IsControl(e) || !e->Focus(true)) continue;
        g.last_focus = e;
        PointAt(e);
        patch::Log("MenuMaX: first move (%s): %s#%s picked", from, e->GetTagName().c_str(), e->GetId().c_str());
        return;
      }
    }
  }
  const int mods = g.shift ? KM_SHIFT : 0;
  Rml::Element* before = g.context->GetFocusElement();
  // Arrow keys / d-pad between controls: MenuMaX's own pick (RmlUi's
  // skips controls that overlap a little, or picks far ones), unless the
  // control takes them itself (sliders, lists, text) or sets nav-up etc.
  bool moved = false;
  if (g.doc && Repeats(k) && k != Rml::Input::KI_TAB && IsControl(before) && !TakesArrows(before) &&
      !NavSet(before, k))
    moved = NavMove(before, k);
  if (!moved) {
    g.context->ProcessKeyDown(k, mods);
    g.context->ProcessKeyUp(k, mods);
    if (g.doc && Repeats(k) && k != Rml::Input::KI_TAB && g.context->GetFocusElement() == before && IsControl(before))
      NavMove(before, k);
  }
  if (g.doc) KeepFocus("key");
  if (g.doc && Repeats(k) && g.context->GetFocusElement() != before && IsControl(g.context->GetFocusElement()))
    PointAt(g.context->GetFocusElement());
  if (GetConfig().menu_log) {
    Rml::Element* f = g.context->GetFocusElement();
    patch::Log("MenuMaX: key %d (%s) -> focus %s#%s", static_cast<int>(k), from, f ? f->GetTagName().c_str() : "-",
               f ? f->GetId().c_str() : "");
  }
}

void Hold(Rml::Input::KeyIdentifier k, bool down) {
  if (down) {
    g.held = k;
    g.held_since = g.held_last = GetTickCount();
  } else if (g.held == k) {
    g.held = Rml::Input::KI_UNKNOWN;
  }
}

// Controllers: 0 up, 1 right, 2 down, 3 left, -1 none.
int PovDir(DWORD pov) {
  if (LOWORD(pov) == 0xFFFF) return -1;
  return static_cast<int>(((pov + 4500) % 36000) / 9000);
}
const Rml::Input::KeyIdentifier kDirKeys[4] = {Rml::Input::KI_UP, Rml::Input::KI_RIGHT, Rml::Input::KI_DOWN,
                                               Rml::Input::KI_LEFT};

void PadDir(int dir, const char* from) {
  if (dir == g.pad_dir_prev) return;
  if (g.pad_dir_prev >= 0) Hold(kDirKeys[g.pad_dir_prev], false);
  g.pad_dir_prev = dir;
  if (dir >= 0) {
    Press(kDirKeys[dir], from);
    Hold(kDirKeys[dir], true);
  }
}

void PollPads() {
  // Gamepads first. DirectInput joysticks (POV + buttons 1 / 2) only without
  // the ControllerHub and while no XInput pad is there: an Xbox pad shows up
  // as both, and the hub's own joystick can be the keyboard (KeyboardPad).
  bool xpad = false;
  // Every pad's state in XInput's layout: from the ControllerHub (SDL3: every
  // make of gamepad) when it runs, otherwise from XInput's four slots.
  std::vector<hub::PadState> pads;
  const bool from_hub = hub::ReadAll(&pads);
  if (!from_hub) {
    if (XInputGetStateFn get = LoadXInput()) {
      for (DWORD i = 0; i < 4; ++i) {
        XPad s{};
        if (get(i, &s) != ERROR_SUCCESS) continue;
        pads.push_back(hub::PadState{s.lx, s.ly, s.rx, s.ry, s.lt, s.rt, s.buttons});
      }
    }
  }
  if (from_hub != g.pads_from_hub || pads.size() != g.pad_count) {
    g.pads_from_hub = from_hub;
    g.pad_count = pads.size();
    patch::Log("MenuMaX: %u controller(s) through %s", static_cast<unsigned>(pads.size()),
               from_hub ? "the ControllerHub (SDL3)" : "XInput");
  }
  // Every connected pad and joystick counts (a keyboard can show up as a pad
  // too); the first one held in a direction steers.
  int dir = -1;
  const char* dir_from = "controller";
  const bool settling = GetTickCount() - g.opened_at < 200;  // held when the screen opened
  if (!pads.empty()) {
    xpad = true;
    for (size_t i = 0; i < pads.size() && i < 8; ++i) {
      const hub::PadState& s = pads[i];
      const WORD pressed = settling ? 0 : static_cast<WORD>(s.buttons & ~g.pad_prev[i]);
      g.pad_prev[i] = s.buttons;
      if (settling) continue;
      int d = -1;
      if (s.buttons & 0x0001) d = 0;
      else if (s.buttons & 0x0008) d = 1;
      else if (s.buttons & 0x0002) d = 2;
      else if (s.buttons & 0x0004) d = 3;
      else if (s.ly > 20000) d = 0;
      else if (s.lx > 20000) d = 1;
      else if (s.ly < -20000) d = 2;
      else if (s.lx < -20000) d = 3;
      if (dir < 0) dir = d;
      if (pressed & 0x1000) Press(Rml::Input::KI_RETURN, "controller A");
      if (pressed & 0x2000) Press(Rml::Input::KI_ESCAPE, "controller B");
      if (pressed && GetConfig().menu_log) patch::Log("MenuMaX: controller %u buttons %04x", static_cast<unsigned>(i), s.buttons);
    }
  }
  // Wheels and other joysticks: through SDL with the hub (hat or centred
  // stick, button 1 = select, button 2 = back), else through the game's own
  // DirectInput reads (only while no XInput pad is there: an Xbox pad shows
  // up as both, and the hub's joystick can be the keyboard).
  std::vector<hub::StickState> sticks;
  if (from_hub) {
    hub::ReadSticks(&sticks);
    if (sticks.size() != g.stick_count) {
      g.stick_count = sticks.size();
      patch::Log("MenuMaX: %u other joystick(s) through SDL", static_cast<unsigned>(sticks.size()));
    }
  } else if (!xpad && g.joy_seen) {
    sticks.push_back(hub::StickState{PovDir(g.joy_pov), g.joy_buttons});
  }
  for (size_t i = 0; i < sticks.size() && i < 8; ++i) {
    const DWORD pressed = settling ? 0 : sticks[i].buttons & ~g.stick_prev[i];
    g.stick_prev[i] = sticks[i].buttons;
    if (settling) continue;
    if (dir < 0 && sticks[i].dir >= 0) {
      dir = sticks[i].dir;
      dir_from = "joystick";
    }
    if (pressed & 1) Press(Rml::Input::KI_RETURN, "joystick button 1");
    if (pressed & 2) Press(Rml::Input::KI_ESCAPE, "joystick button 2");
  }
  if (!settling && (!pads.empty() || !sticks.empty())) PadDir(dir, dir_from);
}

void DoInput(const RECT& game_rect) {
  std::vector<State::Key> keys;
  std::vector<Rml::Character> chars;
  std::vector<std::pair<int, bool>> buttons;
  int wheel;
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    keys.swap(g.keys);
    chars.swap(g.chars);
    buttons.swap(g.buttons);
    wheel = g.wheel;
    g.wheel = 0;
  }
  // A key or button already down when the screen opened (the one that
  // opened it) is not a press on the screen.
  if (GetTickCount() - g.opened_at < 200) {
    for (auto& k : keys) k.down = false;
    for (auto& b : buttons) b.second = false;
    g.joy_buttons = 0;
  }
  // The pointer starts where the game's cursor was, then moves by the
  // mouse movement the game no longer gets: one unit = one menu pixel (as
  // the game's cursor), in window pixels.
  const float dp = (game_rect.bottom - game_rect.top) / 480.0f;
  int mdx, mdy;
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    mdx = g.dx;
    mdy = g.dy;
    g.dx = g.dy = 0;
  }
  if (!g.pointer_set) {
    POINT c{};
    if (ModePromptCursor(&c)) {
      g.px = game_rect.left + (c.x + 0.5f) * dp;
      g.py = game_rect.top + (c.y + 0.5f) * dp;
    } else {
      g.px = (game_rect.left + game_rect.right) * 0.5f;
      g.py = (game_rect.top + game_rect.bottom) * 0.5f;
    }
    g.pointer_set = true;
    patch::Log("MenuMaX: pointer starts at %.0f,%.0f (game cursor %s)", g.px, g.py, ModePromptCursor(&c) ? "seen" : "not seen");
    mdx = mdy = 1;  // send a first position
  }
  if (mdx || mdy) {
    g.px = std::min(std::max(g.px + mdx * dp, 0.0f), static_cast<float>(g.context->GetDimensions().x - 1));
    g.py = std::min(std::max(g.py + mdy * dp, 0.0f), static_cast<float>(g.context->GetDimensions().y - 1));
    g.mouse_x = static_cast<int>(g.px);
    g.mouse_y = static_cast<int>(g.py);
    g.context->ProcessMouseMove(g.mouse_x, g.mouse_y, 0);
  }
  for (const auto& b : buttons) {
    if (b.second) g.context->ProcessMouseButtonDown(b.first, 0);
    else g.context->ProcessMouseButtonUp(b.first, 0);
    if (b.second && b.first == 0) {
      Rml::Element* h = g.context->GetHoverElement();
      if (GetConfig().menu_log || g.clicks < 20)
        patch::Log("MenuMaX: click at %d,%d on %s#%s", g.mouse_x, g.mouse_y, h ? h->GetTagName().c_str() : "-",
                   h ? h->GetId().c_str() : "");
    }
    if (b.second && b.first == 1) Press(Rml::Input::KI_ESCAPE, "right mouse button");
    if (!g.doc) return;
    if (b.first == 0) KeepFocus("click");
  }
  if (wheel) g.context->ProcessMouseWheel(Rml::Vector2f(0.0f, -wheel / 120.0f), 0);
  for (const auto& k : keys) {
    if (k.id == Rml::Input::KI_LSHIFT || k.id == Rml::Input::KI_RSHIFT) {
      g.shift = k.down;
      continue;
    }
    if (k.down) Press(k.id, "keyboard");
    if (!g.context || !g.active) return;
    if (Repeats(k.id)) Hold(k.id, k.down);
  }
  // Typed text goes to a focused edit box.
  if (!chars.empty() && g.context && GetTickCount() - g.opened_at >= 200) {
    Rml::Element* f = g.context->GetFocusElement();
    if (f && f->GetTagName() == "input" && f->GetAttribute<Rml::String>("type", "") == "text")
      for (Rml::Character c : chars) g.context->ProcessTextInput(c);
  }
  PollPads();
  // Held direction: repeat after 400 ms, every 90 ms.
  const DWORD now = GetTickCount();
  if (g.held != Rml::Input::KI_UNKNOWN && now - g.held_since > 400 && now - g.held_last >= 90) {
    g.held_last = now;
    Press(g.held, "repeat");
  }
}

// The screen's pointer: UI\MaX\images\pointer.png, its top left corner
// at the pointer, 22dp tall.
// Where a frame is drawn: the Direct3D 11 swap chain or the Direct3D 9
// back buffer.
struct Target {
  ID3D11RenderTargetView* rtv = nullptr;
  IDirect3DSurface9* surf9 = nullptr;
  UINT w = 0, h = 0;
  const dd::BlitParams* frame = nullptr;  // how the game's picture was drawn
};

// game-window parts: the game's own picture at the control's place, drawn
// again over the screen (pictures the game draws itself, e.g. track previews).
void DrawGameWindows(const Target& t) {
  if (!t.frame || !t.frame->HasSrc()) return;
  const dd::BlitParams& f = *t.frame;
  const LONG sw = f.src_rect.right - f.src_rect.left, sh = f.src_rect.bottom - f.src_rect.top;
  if (sw <= 0 || sh <= 0) return;
  const double sx = static_cast<double>(f.dst_rect.right - f.dst_rect.left) / sw;
  const double sy = static_cast<double>(f.dst_rect.bottom - f.dst_rect.top) / sh;
  // An open drop-down list is drawn by the screen and can lie over the
  // window (the track list opens upwards over the preview): no window then.
  for (GameBind& b : g.game) {
    auto* sel = b.ui == GameBind::kSelect ? dynamic_cast<Rml::ElementFormControlSelect*>(b.el) : nullptr;
    if (sel && sel->IsSelectBoxVisible()) return;
  }
  for (GameBind& b : g.game) {
    if (!b.window) continue;
    const bool show = b.seen && b.last.shown && b.el->IsVisible(true) && b.el->GetOwnerDocument() &&
                      b.el->GetOwnerDocument()->IsVisible();
    const RECT r = b.last.rect;  // left, top, right, bottom in menu pixels
    if (!show || r.right <= r.left || r.bottom <= r.top) {
      b.window_logged = false;
      b.window_frames = 0;
      continue;
    }
    if (b.window_frames++ == 0) b.window_since = GetTickCount();
    if (b.window_frames < 4 || GetTickCount() - b.window_since < 120) continue;  // the game has not drawn it yet
    dd::BlitParams p = f;
    p.src_rect = {f.src_rect.left + r.left, f.src_rect.top + r.top, f.src_rect.left + r.right, f.src_rect.top + r.bottom};
    p.dst_rect = {f.dst_rect.left + static_cast<LONG>(r.left * sx + 0.5), f.dst_rect.top + static_cast<LONG>(r.top * sy + 0.5),
                  f.dst_rect.left + static_cast<LONG>(r.right * sx + 0.5), f.dst_rect.top + static_cast<LONG>(r.bottom * sy + 0.5)};
    if (t.surf9) {
      p.dst9 = t.surf9;
      dd::Blit9(p);
    } else {
      p.dst = t.rtv;
      dd::Blit(p);
    }
    if (!b.window_logged)
      patch::Log("MenuMaX: game window %s: the game's picture %ld,%ld %ldx%ld drawn at %ld,%ld %ldx%ld", b.name.c_str(), r.left,
                 r.top, r.right - r.left, r.bottom - r.top, p.dst_rect.left, p.dst_rect.top, p.dst_rect.right - p.dst_rect.left,
                 p.dst_rect.bottom - p.dst_rect.top);
    b.window_logged = true;
  }
}

void BeginDraw(const Target& t) {
  if (g.r9) {
    g.r9->Begin(t.surf9, t.w, t.h);
  } else {
    dd::ContextTouched();
    g.r11->Begin(dd::B().ctx, t.rtv, t.w, t.h);
  }
}

void EndDraw() {
  g.render->End();
  if (g.r11) dd::ContextTouched();
}

void DrawPointer(const Target& t, float dp) {
  if (!g.pointer_set) return;
  if (!g.pointer_tried) {
    g.pointer_tried = true;
    g.pointer_tex = g.render->LoadTexture(g.pointer_dims, Narrow(UiDir()) + "images\\pointer.png");
    if (!g.pointer_tex) patch::Log("MenuMaX: no images\\pointer.png; the screen has no pointer");
  }
  if (!g.pointer_tex || g.pointer_dims.y <= 0) return;
  const float hgt = 22.0f * dp, wid = hgt * g.pointer_dims.x / g.pointer_dims.y;
  const Rml::ColourbPremultiplied white(255, 255, 255, 255);
  Rml::Vertex v[4] = {{{0, 0}, white, {0, 0}}, {{wid, 0}, white, {1, 0}}, {{wid, hgt}, white, {1, 1}}, {{0, hgt}, white, {0, 1}}};
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  Rml::CompiledGeometryHandle geo = g.render->CompileGeometry(Rml::Span<const Rml::Vertex>(v, 4), Rml::Span<const int>(idx, 6));
  if (!geo) return;
  BeginDraw(t);
  g.render->RenderGeometry(geo, Rml::Vector2f(std::floor(g.px), std::floor(g.py)), g.pointer_tex);
  EndDraw();
  g.render->ReleaseGeometry(geo);
}

// Once, 1.5 s after a screen loads: where every element with an id is and
// whether it is drawn (finding screens that show up blank).
void DumpElement(Rml::Element* e, int depth, int& lines) {
  if (lines >= 400) return;
  if (!e->GetId().empty()) {
    ++lines;
    const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Property* dec = e->GetLocalProperty("decorator");
    patch::Log("MenuMaX: element %*s%s: %.0f,%.0f %.0fx%.0f px, %s%s, opacity %.2f, display %d, visibility %d, z %.0f%s%s",
               depth * 2, "", e->GetId().c_str(), at.x, at.y, size.x, size.y, e->IsVisible(true) ? "visible" : "NOT visible",
               e->IsClassSet("game-hidden") ? " (game-hidden)" : "", e->GetComputedValues().opacity(),
               static_cast<int>(e->GetComputedValues().display()), static_cast<int>(e->GetComputedValues().visibility()),
               e->GetZIndex(), dec ? ", decorator " : "", dec ? dec->ToString().c_str() : "");
  }
  for (int i = 0; i < e->GetNumChildren(); ++i) DumpElement(e->GetChild(i), depth + (e->GetId().empty() ? 0 : 1), lines);
}

void DumpScreens() {
  int lines = 0;
  patch::Log("MenuMaX: elements of the open screens (%u layers), %u scissor rectangles last frame:",
             static_cast<unsigned>(g.layers.size()), g.render->GetStats().scissors);
  for (Layer& l : g.layers)
    if (l.doc) {
      patch::Log("MenuMaX: screen %s%s, %d children", l.file.c_str(), l.doc->IsVisible() ? "" : " (NOT visible)", l.doc->GetNumChildren());
      DumpElement(l.doc, 0, lines);
    }
  g.render->log_scissors = 40;  // and the next frame's clipping
}

void Report(bool force) {
  const DWORD now = GetTickCount();
  if (!force && now - g.last_report < 5000) return;
  g.last_report = now;
  Renderer::Stats& s = g.render->GetStats();
  patch::Log("MenuMaX: %s: %u frames, %u draws, %u new geometry, %u textures (%u failed), %u keys, %u clicks, %u saves, "
             "%u reloads, %u mouse moves, pointer %d,%d, %u doubled key presses ignored",
             g.open_name.c_str(), g.frames, s.draws, s.geometry, s.textures, s.texture_failures, g.key_events, g.clicks,
             g.saves, g.reloads, g.moves, g.mouse_x, g.mouse_y, g.dup_keys);
  s = Renderer::Stats{};
  g.frames = g.key_events = g.clicks = g.saves = g.reloads = g.moves = g.dup_keys = 0;
}

}  // namespace

// ---------------------------------------------------------------- API

void Install() {
  const Config& c = GetConfig();
  if (!c.menu_max) {
    patch::Log("MenuMaX: off (MenuMaX=0)");
    return;
  }
  g.installed = true;
  if (c.max_menus) {
    // The game's "show a dialog" (0x486a00, all 58 places that show one):
    // counted, so a copy is made for the very frame the game's dialog first
    // shows in (in races the dialogs are otherwise not looked at).
    // sub esp,10h; push ebx; mov ebx,[esp+20h]
    static const uint8_t orig[] = {0x83, 0xEC, 0x10, 0x53, 0x8B, 0x5C, 0x24, 0x20};
    if (patch::Matches(0x486A00, orig, sizeof orig)) {
      patch::Jump(0x486A00, reinterpret_cast<uintptr_t>(&MaxDialogShowHook), sizeof orig);
      patch::Log("MenuMaX: the game's dialog show (486a00) hooked");
    } else {
      patch::Log("MenuMaX: the game's dialog show (486a00) NOT hooked (bytes differ); dialogs in races are looked for 20 times a second");
    }
    // push -1; push 550526h (the constructor's own start)
    static const uint8_t ctor[] = {0x6A, 0xFF, 0x68, 0x26, 0x05, 0x55, 0x00};
    if (patch::Matches(0x46ACE0, ctor, sizeof ctor)) {
      patch::Jump(0x46ACE0, reinterpret_cast<uintptr_t>(&MaxDialogNewHook), sizeof ctor);
      patch::Log("MenuMaX: the game's dialog constructor (46ace0) hooked");
    } else {
      patch::Log("MenuMaX: the game's dialog constructor (46ace0) NOT hooked (bytes differ)");
    }
  }
  const DWORD attr = GetFileAttributesW((UiDir() + L"settings.rml").c_str());
  patch::Log("MenuMaX: on; screens in %ls (settings.rml %s); RmlUi starts when a screen first opens", UiDir().c_str(),
             attr == INVALID_FILE_ATTRIBUTES ? "MISSING" : "found");
}

bool Active() { return g.active && !g.capture; }

bool IsOpen(const char* name) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  return g.active && !g.mirror && (g.want_open == name || (g.want_open.empty() && g.open_name == name));
}

bool Open(const char* name, const char* why) {
  if (!g.installed) return false;
  if (g.failed) {
    patch::Log("MenuMaX: %s not opened (%s): RmlUi could not start earlier", name, why);
    return false;
  }
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.want_open = name;
  g.want_close = false;
  g.active = true;
  g.keys.clear();
  g.buttons.clear();
  std::memset(g.key_down, 0, sizeof g.key_down);
  g.last_key = Rml::Input::KI_UNKNOWN;
  g.last_key_source = -1;
  patch::Log("MenuMaX: open %s (%s)", name, why);
  return true;
}

void CloseScreen(const char* name, const char* why) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  const std::string open = g.want_open.empty() ? g.open_name : g.want_open;
  if (g.active && open == name) Close(why);
}

void Close(const char* why) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  if (!g.active) return;
  g.want_close = true;
  g.want_open.clear();
  g.active = false;
  g.held = Rml::Input::KI_UNKNOWN;
  g.pad_dir_prev = -1;
  patch::Log("MenuMaX: close %s (%s)", g.open_name.c_str(), why);
}

namespace {

// The tooltip (title=) of the control under the pointer, after it rests
// there for half a second, in #max-tooltip of that control's screen.
void Tooltip(float dp) {
  const Rml::Element* owner = nullptr;
  std::string text;
  if (!g.capture && g.pointer_set)
    for (Rml::Element* e = g.context->GetHoverElement(); e; e = e->GetParentNode())
      if (e->HasAttribute("title")) {
        owner = e;
        text = e->GetAttribute<Rml::String>("title", "");
        break;
      }
  const DWORD now = GetTickCount();
  if (owner != g.tip_owner) {
    g.tip_owner = owner;
    g.tip_since = now;
    g.tip_logged = false;
  }
  const bool show = owner && !text.empty() && now - g.tip_since >= 500;
  Rml::ElementDocument* in = show ? const_cast<Rml::Element*>(owner)->GetOwnerDocument() : nullptr;
  for (Rml::ElementDocument* d : Docs()) {
    Rml::Element* tip = d->GetElementById("max-tooltip");
    if (d != in) {
      if (tip) tip->SetProperty("display", "none");
      continue;
    }
    if (!tip) {
      Rml::ElementPtr made = d->CreateElement("div");
      made->SetId("max-tooltip");
      tip = d->AppendChild(std::move(made));
    }
    const Rml::String rml = Rml::StringUtilities::EncodeRml(text);
    if (tip->GetInnerRML() != rml) tip->SetInnerRML(rml);
    const Rml::Vector2f at = d->GetAbsoluteOffset(Rml::BoxArea::Border);
    const float width = static_cast<float>(g.context->GetDimensions().x);
    // Kept inside the window (its size from the last layout).
    const Rml::Vector2f size = tip->GetBox().GetSize(Rml::BoxArea::Border);
    const float height = static_cast<float>(g.context->GetDimensions().y);
    float x = g.mouse_x - at.x + 4.0f * dp, y = g.mouse_y - at.y + 22.0f * dp;
    if (x + at.x + size.x > width - 2.0f * dp) x = width - 2.0f * dp - size.x - at.x;
    if (x + at.x < 0) x = -at.x;
    if (y + at.y + size.y > height) y = g.mouse_y - at.y - 4.0f * dp - size.y;
    tip->SetProperty("left", Rml::ToString(x) + "px");
    tip->SetProperty("top", Rml::ToString(y) + "px");
    tip->SetProperty("display", "block");
    if (!g.tip_logged && GetConfig().menu_log) patch::Log("MenuMaX: tooltip \"%s\"", text.c_str());
    g.tip_logged = true;
  }
}

void PresentFrame(const Target& t, const RECT& game_rect) {
  const UINT w = t.w, h = t.h;
  if (!g.installed || g.failed) return;
  std::string open;
  bool close;
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    open.swap(g.want_open);
    close = g.want_close;
    g.want_close = false;
  }
  if (close && g.doc) {
    CloseDoc();
    MenusRefreshFromIni();
    Report(true);
    patch::Log("MenuMaX: screen closed after %lu ms", static_cast<unsigned long>(GetTickCount() - g.opened_at));
  }
  if (!open.empty()) {
    if (!InitRml()) {
      Close("RmlUi could not start");
      return;
    }
    g.opened_at = GetTickCount();
    g.last_report = g.opened_at;
    g.pointer_set = false;
    if (!LoadDoc(open, std::string())) {
      Close("screen could not be loaded");
      return;
    }
    std::lock_guard<std::recursive_mutex> l(g.mu);
    g.active = true;  // (closing the game menu copies for it let go)
  }
  if (!RaceRunning()) {
    Mirror();
  } else if (g.mirror || g_max_dialog_shows != g.dialog_shows_seen || GetTickCount() - g.race_mirror_at >= 50) {
    g.dialog_shows_seen = g_max_dialog_shows;
    // In a race: the pause box shows its copy as soon as the game shows it,
    // and a copy goes as soon as the game's box does (Restart Race left the
    // copy up, tied to a box that was gone). Looked at 20 times a second
    // while nothing is shown, every frame while a copy is.
    g.race_mirror_at = GetTickCount();
    Mirror();
  }
  if (!g.doc || !g.context) return;
  SyncGame();
  if (g.capture) {
    // The game reads input itself for now: none of it is a press here.
    std::lock_guard<std::recursive_mutex> l(g.mu);
    g.keys.clear();
    g.chars.clear();
    g.buttons.clear();
    g.dx = g.dy = 0;
    g.wheel = 0;
    g.held = Rml::Input::KI_UNKNOWN;
  }
  // Sizes: the window in pixels, 1dp = one 640x480 menu pixel.
  const Rml::Vector2i dims(static_cast<int>(w), static_cast<int>(h));
  if (g.context->GetDimensions() != dims) {
    g.context->SetDimensions(dims);
    patch::Log("MenuMaX: window %ux%u, menu area %ld,%ld %ldx%ld", w, h, game_rect.left, game_rect.top,
               game_rect.right - game_rect.left, game_rect.bottom - game_rect.top);
  }
  const float dp = (game_rect.bottom - game_rect.top) / 480.0f;
  if (std::fabs(g.context->GetDensityIndependentPixelRatio() - dp) > 0.001f) {
    g.context->SetDensityIndependentPixelRatio(dp);
    patch::Log("MenuMaX: 1dp = %.3f window pixels", dp);
  }
  DoInput(game_rect);
  if (!g.doc) return;  // closed by a key
  Watch();
  if (!g.doc) return;
  if (!g.pending.empty() && GetTickCount() - g.last_change > 300) FlushPending("settled");
  Tooltip(dp);
  g.context->Update();
  BeginDraw(t);
  g.context->Render();
  EndDraw();
  DrawGameWindows(t);
  DrawPointer(t, dp);
  ++g.frames;
  if (g.dump_at && static_cast<int>(GetTickCount() - g.dump_at) >= 0) {
    g.dump_at = 0;
    DumpScreens();
  }
  Report(false);
}

}  // namespace

void DialogMade(const char* name) {
  if (!g.installed || g.failed || !GetConfig().max_menus || !name) return;
  char buf[64];
  size_t n = 0;
  if (IsBadStringPtrA(name, sizeof buf)) return;
  for (; n + 1 < sizeof buf && name[n]; ++n) buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[n])));
  buf[n] = 0;
  std::string dtm(buf);
  const size_t slash = dtm.find_last_of("\\/");
  if (slash != std::string::npos) dtm.erase(0, slash + 1);
  if (dtm.size() > 4 && dtm.compare(dtm.size() - 4, 4, ".dtm") == 0) dtm.resize(dtm.size() - 4);
  if (!g.index_read) ReadIndex();
  if (!g.index.count(dtm)) return;
  for (const Layer& l : g.layers)
    if (std::find(l.dialogs.begin(), l.dialogs.end(), dtm) != l.dialogs.end()) return;  // its copy is up already
  g.hold_dtm = dtm;
  g.hold_since = GetTickCount();
  g.held_frames = 0;
  if (++g.holds <= 40 || GetConfig().menu_log) patch::Log("MenuMaX: game dialog %s made: the screen is held until its copy is up", dtm.c_str());
}

bool HoldPresent() {
  if (g.hold_dtm.empty()) return false;
  const DWORD waited = GetTickCount() - g.hold_since;
  bool up = false;
  for (const Layer& l : g.layers)
    if (l.doc && std::find(l.dialogs.begin(), l.dialogs.end(), g.hold_dtm) != l.dialogs.end()) up = true;
  if (up || waited > 250) {
    if (g.holds <= 40 || GetConfig().menu_log)
      patch::Log("MenuMaX: game dialog %s: %s after %lu ms, %d frame(s) held", g.hold_dtm.c_str(),
                 up ? "copy up" : "copy not up (gave up)", static_cast<unsigned long>(waited), g.held_frames);
    g.hold_dtm.clear();
    return false;
  }
  ++g.held_frames;
  return true;
}

void Present11(ID3D11RenderTargetView* rtv, UINT w, UINT h, const RECT& game_rect, const dd::BlitParams* frame) {
  if (!g.installed || g.failed || dd::IsD3D9()) return;
  Target t;
  t.frame = frame;
  t.rtv = rtv;
  t.w = w;
  t.h = h;
  PresentFrame(t, game_rect);
}

void Present9(IDirect3DSurface9* back, UINT w, UINT h, const RECT& game_rect, const dd::BlitParams* frame) {
  if (!g.installed || g.failed || !dd::IsD3D9() || !back) return;
  Target t;
  t.frame = frame;
  t.surf9 = back;
  t.w = w;
  t.h = h;
  PresentFrame(t, game_rect);
}

void DeviceGone9() {
  if (g.r9) g.r9->DeviceGone();
}

void KeyEvent(unsigned dik, bool down) {
  const Rml::Input::KeyIdentifier k = FromDik(dik);
  std::lock_guard<std::recursive_mutex> l(g.mu);
  // The same change reported twice (two ways of reading the keyboard).
  if (dik < 256) {
    if (g.key_down[dik] == down) return;
    g.key_down[dik] = down;
  }
  // Typed text, by the keyboard layout (edit boxes).
  if (down && dik < 256 && g.active) {
    const bool shift = g.key_down[0x2A] || g.key_down[0x36];
    const bool ctrl = g.key_down[0x1D] || g.key_down[0x9D];
    const UINT vk = MapVirtualKeyA(dik & 0x7F, 1 /* MAPVK_VSC_TO_VK */);
    if (vk && !ctrl) {
      BYTE ks[256] = {};
      if (shift) ks[VK_SHIFT] = 0x80;
      if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
      if (g.key_down[0xB8]) ks[VK_CONTROL] = ks[VK_MENU] = 0x80;  // AltGr
      wchar_t buf[4] = {};
      const int n = ToUnicode(vk, dik, ks, buf, 4, 0);
      for (int i = 0; i < n && i < 4; ++i)
        if (buf[i] >= 32 && buf[i] != 127 && g.chars.size() < 64) g.chars.push_back(static_cast<Rml::Character>(buf[i]));
    }
  }
  if (k == Rml::Input::KI_UNKNOWN) return;
  if (g.keys.size() < 256) g.keys.push_back({k, down});
}

void MouseButton(int button, bool down) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  if (g.buttons.size() < 64) g.buttons.emplace_back(button, down);
}

void MouseMove(int dx, int dy) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.dx += dx;
  g.dy += dy;
  ++g.moves;
}

void MouseWheel(int delta) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.wheel += delta;
}

void JoyState(DWORD pov, DWORD buttons) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.joy_seen = true;
  g.joy_pov = pov;
  g.joy_buttons = buttons;
}

}  // namespace ui

extern "C" void __cdecl MaxDialogNew(const char* name) { ui::DialogMade(name); }
