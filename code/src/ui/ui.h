// MenuMaX=1: new menu screens made with RmlUi (HTML/CSS-style text files,
// https://github.com/mikke89/RmlUi) from UI\MaX\ next to Mcm2.exe.
//
// - Screens are .rml (HTML-like) + .rcss (CSS-like) files; pictures are PNG.
//   They are drawn at the window's resolution over the game's 640x480 menus.
//   Lengths in "dp" are 640x480 menu pixels (1dp = window height / 480).
// - Files are watched while a screen is open: saving a changed .rml, .rcss
//   or picture reloads the screen at once (live editing).
// - Settings need no code: <input type="checkbox" ini="VSync"/>,
//   <input type="range" ini="ViewDistance" .../>, <select ini="...">,
//   <span ini-text="Key"/> read and write mcm2max.ini keys directly (see
//   package/UI/MaX/README.txt for every attribute).
// - Every screen works with the mouse, the keyboard (arrows / Tab move,
//   Enter / Space select, Left / Right change sliders, Esc goes back) and
//   controllers (d-pad / left stick move, A select, B back; XInput pads and
//   DirectInput joysticks).
// - While a screen is open it takes all keys, mouse movement and buttons and
//   controller buttons; the game's cursor stays put (so the page under the
//   screen does not react) and the screen draws its own pointer
//   (UI\MaX\images\pointer.png).
//
// Drawn with Direct3D 11 (render_d3d11.cpp) or, with Renderer=D3D9 (and on
// Windows XP), Direct3D 9 (render_d3d9.cpp). Without -DMCM2MaX_RMLUI the DLL
// has stubs.
#pragma once
#include <windows.h>

struct ID3D11RenderTargetView;
struct IDirect3DSurface9;

namespace dd {
struct BlitParams;
}

namespace ui {

// At start-up, after the config is loaded.
void Install();
// A screen is open (input goes to it).
bool Active();
// Opens UI\MaX\<name>.rml over the menus (game thread). False if it can't.
bool Open(const char* name, const char* why);
void Close(const char* why);
// Closes the screen only if it is `name`.
void CloseScreen(const char* name, const char* why);
// The screen `name` (UI\\MaX\\<name>.rml) is open (or opening).
bool IsOpen(const char* name);

// Present (Direct3D 11, under the renderer lock): draws the open screen over
// the frame; game_rect is where the 640x480 picture went in the window.
// `frame` is how the game's picture was drawn (game-window parts of a screen
// show it through: it is drawn again there, over the screen).
void Present11(ID3D11RenderTargetView* rtv, UINT w, UINT h, const RECT& game_rect, const dd::BlitParams* frame = nullptr);
// The same for Direct3D 9: drawn into the back buffer `back` (w x h).
void Present9(IDirect3DSurface9* back, UINT w, UINT h, const RECT& game_rect, const dd::BlitParams* frame = nullptr);
// The Direct3D 9 device is about to be released (a new one follows).
void DeviceGone9();

// A game dialog (0x46ace0) was just made; presents are held (HoldPresent
// true) until MaXMenus has its copy up, at most 250 ms.
void DialogMade(const char* name);
bool HoldPresent();

// Input taken from the game while a screen is open (any thread).
// DirectInput key codes (DIK_*), mouse buttons 0-7, wheel in DirectInput
// units (120 per notch), joystick POV (hundredths of degrees, 0xFFFFFFFF =
// centred) and buttons (bit per button).
void KeyEvent(unsigned dik, bool down);
void MouseButton(int button, bool down);
void MouseMove(int dx, int dy);
void MouseWheel(int delta);
void JoyState(DWORD pov, DWORD buttons);

}  // namespace ui
