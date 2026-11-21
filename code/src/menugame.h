// MenuMaX: the game's own menu controls, found by name on the screens shown
// now, read and changed for a MenuMaX screen drawn over them (ui/ui.cpp,
// game="Name"). Reads happen at once (game thread); changes are queued and
// made at the start of the game's next tick (outside the menu code, where a
// click may close or replace a page), the way the game's own clicks go.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mg {

enum Kind { kUnknown, kButton, kCheck, kRadio, kDropdown, kEdit, kList, kSlider, kScrollArrow, kStatic };

struct Info {
  Kind kind = kUnknown;
  bool shown = false;    // shown and taking input (the game's own test), not mid show/hide animation
  bool enabled = false;  // not greyed out
  int value = 0;         // checkbox / radio / multi-state: state; slider: position; list / dropdown: chosen row (-1 none)
  int range = 0;         // multi-state: number of states; slider: highest position; list / dropdown: rows
  std::string text;      // the control's text (labels, a multi-state's state text, an edit box's text)
  RECT rect{};           // in 640x480 menu pixels
  uintptr_t dialog = 0;
  int top = 0, visible = 0;  // list / dropdown: first row shown, rows shown at once
  bool live = false;         // shown (+0x70), its panels open, maybe mid show / hide animation
  bool tree = false;         // the GUI tree marks it shown
  int anim = 0;              // 1 show animation running, 2 hide animation, 0 none
  int frame = -1, frames = 0;  // the running animation's frame, of frames
};

// Looks at the screens shown now (game thread, at most once a frame).
void Refresh();
// The control called `name` on a shown screen (the topmost modal one first),
// or on dialog `scope` only.
bool Get(const char* name, Info* out, uintptr_t scope = 0);
// Logs where a control of that name is in the whole GUI tree (diagnostics).
void Locate(const char* name);
// Logs the controls the game adds to dialogs that end up unlinked (diagnostics).
// A list's or dropdown's rows.
bool Rows(const char* name, std::vector<std::string>* rows, uintptr_t scope = 0);
// A modal dialog is shown now: its controls' names.
bool Modal(std::vector<std::string>* names);
// The shown dialogs, oldest first, with their named controls (as of the
// last Refresh).
struct Dialog {
  uintptr_t p = 0;
  bool modal = false;
  std::string dtm;  // its layout, lower case without ".dtm" (e.g. "optgame")
  RECT rect{};      // where the game put it: left, top, width, height (+0x160)
  std::vector<std::string> names;
};
void Dialogs(std::vector<Dialog>* out);
// A 32-bit field of a dialog (0 if it can't be read).
int DialogField(uintptr_t dialog, size_t offset);

// Changes, made at the start of the next tick.
// scope: the control on that dialog only (0: as Get finds it).
void Click(const char* name, uintptr_t scope = 0);  // button, checkbox, radio, multi-state, tab
void SetSlider(const char* name, int position, int event, uintptr_t scope = 0);  // event: 15 grab, 3 change, 16 release
void Choose(const char* name, int row, uintptr_t scope = 0);  // list / dropdown row, as clicking it does
void SetText(const char* name, const std::string& text, uintptr_t scope = 0);  // edit box

// framerate.cpp, at the start of the game's tick (game thread).
void Tick();
// fixes_resolution.cpp's menu cursor hook (game thread, every menu frame):
// changes are made here when no tick has come for a while.
void Frame();

}  // namespace mg
