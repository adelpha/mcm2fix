MCM2MaX menu screens (MenuMaX=1 in mcm2max.ini)
================================================

The screens in this folder are drawn by RmlUi (https://mikke89.github.io/RmlUiDoc/),
a library that lays out HTML/CSS-style text files. Open them in any text editor.

  settings.rml   the "More Settings" screen (Options > Graphics > More Settings)
  menus\         copies of the game's own menu screens (MaXMenus=1), one .rml
                 and .rcss each, and menus.rcss (the look they share)
  max.rcss       the look shared by every other screen (colours, sizes, fonts)
  images\        pictures (PNG; pointer.png is the mouse pointer)
  fonts\         fonts (TTF/OTF, all loaded at start)

Live editing: while a screen is open in the game, saving any file in this folder
reloads it at once (F5 reloads too). Mistakes are written to the MCM2MaX log
(%LOCALAPPDATA%\Motocross Madness 2\logs\) as "MenuMaX: RmlUi ..." lines.

Sizes: "dp" is one pixel of the game's 640x480 menus, so 640dp x 480dp is the
menu area at any window size. Text and pictures are drawn at the window's real
resolution, so a picture can be bigger than its dp size for a sharper look.

Settings without code
---------------------
Any control can read and save an mcm2max.ini setting:

  <input type="checkbox" ini="MotionBlur" default="1"/>
  <input type="range" ini="ViewDistance" min="1" max="10" step="0.5" default="1" restart/>
  <select ini="Anisotropy" default="16"><option value="0">Off</option>...</select>

  ini="Key"           the setting it reads and saves
  default="v"         its value when mcm2max.ini doesn't have the key
  ini-on="v"          the value saved when a checkbox is ticked (default 1)
  restart             the change applies the next time the game starts
                      (shows the "* after restarting the game" note)
  game="Name"         a control on the game's own screen under this one (see
                      "Copies of the game's screens" below)
  <span ini-text="Key" suffix="x" zero="Off"/>   shows a setting's value
  enable-if="Key!=0"  (or "Key=1") greys the control unless it holds
  autofocus           where the keyboard / controller starts

Saved values go to mcm2max.ini (or the per-user MCM2MaX-menu.ini if the game
folder can't be written), the same as the Enhancements box.

Buttons: <button onclick="back">Back</button>. Commands: back (or close),
open <screen> (another .rml in this folder), reload.

Mouse, keyboard and controllers
-------------------------------
Every screen can be used with all three, with no extra work:
  move between controls   arrow keys / Tab, d-pad / left stick
  press / tick / open     Enter or Space, controller A (button 1)
  change a slider / list  Left / Right
  back                    Esc, right mouse button, controller B (button 2)
Everything that can be picked needs "nav: auto" and "tab-index: auto" (set for
input, select and button in max.rcss); nav-up / nav-down / nav-left / nav-right
set a different order where the automatic one is wrong.

Copies of the game's screens
----------------------------
The screens in menus\ are drawn over the game's own menus, which keep running
underneath and do all the saving and applying. A screen copies one or more of
the game's dialogs (each is one layout, UI\<NAME>.DAT; tools/mcm2rml.py in the
MCM2MaX source writes a first copy of them):

  <body game-dialogs="optbase optgame">   the game dialogs this screen copies
  game-opaque (on <body>)   it covers the whole menu: nothing under it is drawn
  game-back="Back" (on <body>)   Esc / B / the right mouse button click this
                    control (on the screen's first dialog)
  <div game-dialog="optgame">   the part copying that dialog: shown while the
                    game shows it, moved to where the game put it

While the game shows dialogs, the screens copying them are shown, the game's
top one above the others. When the game shows a dialog no screen copies, the
game's own menus show instead (the log says which: "has no copy in menus\").

Each element is tied to the game's control of the same name, on its dialog:

  game="Name"     shows that control: its text (the game's own, so translated
                  versions show theirs), greyed out (class "disabled"), ticked
                  or picked (class "on"), hidden when the game hides it (class
                  "game-hidden"); a click on the element clicks the control.
                  <input type="range">, <select> and <input type="text"> take
                  the game's slider position, list rows and text, and pass
                  changes back; <div class="list"> shows a list's rows as
                  <div class="row"> (class "on": the chosen one). The text goes
                  into a child of class "t" if there is one; game-text="off"
                  keeps the element's own text.
  game-if="Name"  shown only while the game shows that control
  game-window     (with game="Name") the game's own picture of that control
                  shows through here, e.g. the track preview the game draws

Pictures can be the game's own, read from its UI\<ARCHIVE>.DAT files:
  decorator: image(/dat/OPTBASE/Options/OptTab01.tga);
After a '?': black (black is see-through too), opaque (nothing is), frame=N
(only the outer N pixels: a border-only box plate), bar (a slider track without
its tick marks). Magenta, the game's see-through colour, always is. A PNG of the
same name in UI\MaX\dat\<ARCHIVE>\... (e.g. dat\OPTBASE\Options\OptTab01.png)
is used instead, for sharper art.
