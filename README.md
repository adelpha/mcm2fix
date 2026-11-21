# Motocross Madness 2 MaXimuM - An MCM2 Compatibility and Enhancement Project

Motocross Madness 2 MaXimuM (MCM2MaX) makes **Motocross Madness 2** (Microsoft / Rainbow Studios, 2000) run
on modern Windows by double-clicking the no-CD patched `Mcm2.exe`. You don't need compatibility
settings, administrator rights, DirectX wrappers or old codecs, and you don't need to
change the game's display resolution.

This is a ***work-in-progress*** and will change fast, with frequent bugs until first release candidate.

---

***You need the no-CD `Mcm2.exe`.*** MCM2MaX only works with the widely available no-CD exe (1,577,005 bytes; see [Requirements](#requirements)). The original `Mcm2.exe` from the CD uses SafeDisc copy protection, which Windows 10 and 11 can't run at all, and MCM2MaX doesn't patch it. With any other exe MCM2MaX does nothing.

---

Here's just a few of Motocross Madness 2 MaXimuM's core features:

| Feature | Description |
|---|---|
| Ease of use | Drop the files into your MCM2 directory with the no-CD exe and it just works. |
| No wrappers, codecs, external DLLs or compatibility settings required | Indeo5 decoder built-in for the game's video files, "Modern" rendering on the GPU using D3D11 or, for Windows XP compatibility, D3D9. |
| No physics changes | Game physics runs at a locked 30fps, separated from the renderer. It runs the original game's code straight from its own exe for all physics calculations, guaranteeing a 100% behaviour match with the original game. |
| Performance | With GPU terrain rendering enabled in the D3D11 renderer, it can render a full map at 10x extended view distance with maximum terrain quality across the entire map at [hundreds of frames per second](screenshots/10xview_347fps.png) on a modern GPU. (Though I'd suggest capping the framerate or using vsync!) |
| Graphical enhancements | Widescreen, AA, AF, FoV, Alpha-to-coverage, support for high-resolution replacement textures, environment mapped reflections on bikes and riders, soft shadows for all riders, sharp antialiased text in the menus and HUD and more! |
| Audio | Drop in [DSOAL](https://github.com/kcat/dsoal) and the EAX effects will just work. You can change the EAX 1.0 presets per-event type in the ini file! |
| Music | Optional music tracks, either per-event or per-track (provide your own). |
| UI | Original UI assets and functionality reimplemented with [RmlUi](https://github.com/mikke89/RmlUi) for simple modification and extensibility. New options unique to MCM2MaX added to the menus. |
| Input | Native XInput support with analog and force-feedback-to-rumble translation. SDL3 for wide controller support. |

See the [Features](#features) section for more details.

MCM2MaX is a single DLL that the game loads in place of `d3drm.dll`, a library
Windows no longer ships. When the DLL loads, it patches the game's code in
memory. It brings its own DirectDraw 7 / Direct3D 7 implementation that draws
with **Direct3D 11**, and it falls back to **Direct3D 9** where Direct3D 11 is
missing. Your `Mcm2.exe` and the other game files are never modified.

Fonts intended to be used in the game are included, but are optional and can be swapped out for fonts of your preference (can't guarantee replacements will look good though!)

The same DLL also runs on **Windows XP SP3**, where it uses Direct3D 9.

---

## Contents

- [What it fixes](#what-it-fixes)
- [Features](#features)
- [Requirements](#requirements)
- [Not yet tested](#not-yet-tested)
- [Future work](#future-work)
- [Hypothetical future work](#hypothetical-future-work)
- [Installing](#installing)
- [Where your settings, saves and logs go](#where-your-settings-saves-and-logs-go)
- [Uninstalling](#uninstalling)
- [Renderers and fullscreen](#renderers-and-fullscreen)
- [Windows XP](#windows-xp)
- [Sound, EAX and DSOAL](#sound-eax-and-dsoal)
- [Music](#music)
- [MCM2MaX.ini reference](#mcm2maxini-reference)
- [Texture dumping and replacement](#texture-dumping-and-replacement)
- [Problems and logs](#problems-and-logs)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [Licences](#licences)

---

## What it fixes

These are the problems that stop the original game, or make it misbehave, on
Windows 10 and 11.

| Problem in the original game | What MCM2MaX does |
|---|---|
| The game imports `d3drm.dll`, which Windows 10/11 don't ship, so it won't start. | MCM2MaX *is* `d3drm.dll`. It provides the one function the game used (`D3DRMVectorRotate`). |
| The game quits silently at startup. Right after switching to 640x480, `Flip` returns `DDERR_SURFACELOST` and the game treats it as fatal. | Its renderer never changes the display mode. The flip loop also restores and retries on a lost surface. |
| An administrator check writes to HKLM and creates a temp file in the game folder, and the game exits if either fails. | Removed. The game runs as a standard user. |
| A race between threads on multi-core CPUs hangs the game on the logo or fails startup. | Fixed in the renderer and startup path. `OneCore=1` keeps the old single-core workaround available. |
| Settings are written to `HKLM`, and replays, profiles, saves and logs go into the game folder. Both fail without admin rights, or land in the VirtualStore. | Settings go to `HKCU`. File writes go to a per-user folder that overlays the game folder (see [below](#where-your-settings-saves-and-logs-go)). |
| A game copied off the CD by hand (rather than installed with Setup) quits silently at startup: the licence check needs `EBUEula.dll` and `EULA.rtf` from the CD root. | When either file is missing, the licence check is skipped. |
| With Windows display scaling above 100%, Windows stretches the game (blurry, widescreen modes capped below your real resolution) and the menu mouse drifts, so slow moves can go the wrong way. | The game is declared DPI aware, so it runs at your real resolution (`DpiAware`). |
| A video card picker and a "hardware has changed" dialog appear. | No picker. The best Direct3D adapter is chosen automatically and the dialog is skipped. |
| Saved hardware-test results (`DriverInfo`) could leave the window not redrawing. | The cache is never used: the game's quick card check runs against MCM2MaX's renderer at every start, with the resolution list built from your monitor. |
| Videos (intro, credits, menu previews) need the Indeo 5 codec and DirectShow, which modern Windows don't have. | MCM2MaX has its own AVI player: Indeo 5 through FFmpeg, MS ADPCM sound, played with waveOut. No codec install is needed. |
| In-game Help opens `MCM2HELP.HLP`, and Windows 10/11 can't open WinHelp files. | The Help buttons open `MCM2HELP.html`, a full conversion with pages, pictures, stunt videos, index and search. |
| The game asks for video memory as a signed number, so a card with 2 GB or more makes widescreen modes unusable. | The renderer reports 256 MB. |
| Only 4:3 resolutions are offered. | Widescreen modes up to your desktop size are offered, and the field of view widens to match. |
| Game speed, AI and physics break at high frame rates. | Races run at a fixed 30 simulation steps per second with smooth blended drawing (see [Frame pacing](#frame-pacing-and-smoothness)). |
| **Restart Race** from the pause menu puts riders on the wrong grid spots, facing the wrong way, and FINAL LAP is not shown again. | Fixed (`RestartFix`). |
| The race direction arrow leans toward the screen centre instead of pointing at the next gate. | Fixed, with a slight top-down tilt and a larger size (`Arrow*`). |
| Trees and flags show a stair-step fringe that hides things behind them. | Fixed by alpha-test threshold (`AlphaCut`) or, with MSAA, alpha-to-coverage (`AlphaToCoverage`), and transparent texels no longer bleed grey into leaf edges. |
| Texture formats are always 16-bit, because of a bug in the game's 32-bit format description. | Fixed (`TrueColorTextures`), plus full 32-bit texture replacements. |
| Distant riders' engines play quiet looping snippets of old sounds. The game re-sorts its engine voices on every update with the C library's qsort, which is unstable: riders with equal priority swap places, so their voices are handed around and restarted. | The game's sorts use a stable merge sort (`FastSort`), so voices stay with their rider. |
| With EAX (DSOAL or an EAX sound card), outdoor events ask for "reverb off" with values OpenAL Soft rejects, so whatever reverb was set before (for example Arena after a Supercross race) stays on. | Each outdoor event gets a real outdoor reverb preset (`BajaReverb` and the other `*Reverb` settings). |
| After alt-tabbing out of a race, keys let go while away stay held (the bike keeps steering or accelerating), and the keyboard can stop responding until restart: the game only re-acquires it after one kind of DirectInput error. | Keys and buttons let go while away are released, and the keyboard and joysticks are acquired again when the game is back in front (`AltTabFix`). |
| Computer riders never pick the last bike or rider in the lists: the game's random pick is one short. | Every bike and rider can be picked (`AiPickFix`). |
| Coming back to the game after switching away mid-race, it stalled for seconds and then ran fast to catch up: its clock kept running while it was in the background. | The clock stops while the game is in the background, and leaving mid-race opens the pause menu (`PauseOnFocusLoss`). |
| Xbox controllers steer digitally: Windows reports them to the game as gamepads, and the game turns a gamepad's sticks into on/off presses. Both triggers share one axis, so throttle and brake cancel out, and there is no rumble. | Xbox (XInput) controllers are analog joysticks with separate trigger axes and rumble from the game's force feedback (`XInput`). |
| With DSOAL, alt-tabbing out leaves the last fraction of a second of each sound repeating: the game stops while in the background, and only Windows' own DirectSound silences a background game. | While the game is in the background, MCM2MaX silences it (`BackgroundMute`). |
| DSOAL ignores the game's "mute at max distance" flag, so riders out of earshot can still be heard quietly. | With a replacement DirectSound such as DSOAL, MCM2MaX mutes 3D sounds past their max distance itself (`SoundMaxDistanceMute`). |
| With a long view distance, distant terrain has holes (sky through hills, trees standing on nothing) that come and go as the camera turns. The game only refreshes the small pieces of a terrain block that is wholly in view when its visibility value is exactly one number, but blocks found wholly in view get a different value with the same "wholly in view" bit. | The bit is tested instead, so those pieces are always refreshed (`TerrainHoleFix`). |

## Features

### Display
- **Borderless window at your desktop resolution** on the primary monitor.
  The game's display mode is virtual: the picture is rendered at the
  resolution you choose in the game's options and scaled to the screen with
  its aspect ratio kept. The desktop never changes resolution.
- **Optional exclusive fullscreen** (`Fullscreen=Exclusive`), at the desktop
  mode or any mode the monitor has, refresh rate included, picked in the
  game's Options or named in the ini (`ExclusiveResolution=1024x768@85`).
- **Your monitor's own resolutions**, widescreen included: the game's
  Options list the sizes your monitor supports, up to your desktop size, and
  your exact desktop size is always one of them. With exclusive fullscreen
  every mode is listed with its refresh rate.
- **Starts at your desktop resolution**, and the resolution you pick in
  Options is saved in the ini (`BorderlessResolution` /
  `ExclusiveResolution`), which always wins over the game's own setting.
  "Desktop" at the top of the list goes back to following the desktop.
- **Borderless / Exclusive buttons** in Options > Graphics, switched at once
  (Direct3D 11), each with its own resolution.
- **Widescreen field of view** (`WideFov`): wider screens show more to the
  sides instead of cropping the top and bottom. The speedometer stays round.
  `Fov` sets the base field of view.
- **VSync and frame cap** (`VSync`, `FrameCap`), and a **240 fps cap in the
  menus** (`MenuFpsCap`) so they don't run at thousands of frames a second.

### Graphics
- **Multisample antialiasing** of the 3D scene, up to 8x (`Antialiasing`,
  default 4x), with **alpha-to-coverage** so tree leaves, fences and other
  cut-outs get smooth edges too (`AlphaToCoverage`).
- **Anisotropic filtering**, up to 16x (`Anisotropy`, default 16x).
- **View distance, fog distance and model detail** multipliers
  (`ViewDistance`, `FogDistance`, `ModelLod`), plus texture sharpness
  (`MipBias`).
- **Speed blur** (`MotionBlur`, on by default). At speed, the sides of the
  3D scene streak towards where the bike is heading while the middle and the
  HUD stay sharp. Direct3D 11 and Direct3D 9. Set `MotionBlur=0` to turn it off, or untick it in Options > Graphics.
- **True-colour textures** where the game's own art has more than 16 bits
  (`TrueColorTextures`).
- **Shadows for every rider, drawn by the graphics card** (`RiderShadows`).
  The game drew one blurry shadow, for the rider the camera follows, into a
  small picture on the CPU. Now every rider and bike casts a sharp,
  soft-edged shadow on the ground, drawn on the GPU and moving smoothly with
  the riders between simulation steps (Direct3D 11).
- **Reflections on bikes and helmets** (`Reflections`, on by default). Bike
  bodywork and helmets, and more faintly the riders' suits, get a soft
  reflection of the sky and of the terrain, trees and track objects around
  the bike, brightest at glancing angles, plus a highlight from the sun.
  Strength, softness and shine are tunable per material (`ReflectBike*`,
  `ReflectHelmet*`, `ReflectRider*`). `LiquidMetal=1` turns every bike and
  rider into liquid chrome. Direct3D 11.
- **Texture dumping and replacement** by name with PNG, TGA, JPEG or BMP files of
  any size (`TextureDump`, `TextureReplace`). See
  [below](#texture-dumping-and-replacement).
- **Sharp text**. The game draws its menu and HUD text with GDI on 640x480
  surfaces. MCM2MaX draws that text on the GPU instead, antialiased, at your
  screen's resolution (`GpuText`).
- **New race HUD**. The position, lap and time panels are drawn larger
  (`HudScale`) in new fonts: Contrail One for words and DSEG7 Classic for
  digits (`HudFont`, `HudDigitFont`). The fonts are loaded by the game only
  and nothing is installed in Windows. Rider name labels are larger too
  (`LabelScale`), and the speedometer needle is an antialiased thicker line
  (`LineWidth`).
- **Race title intro** (`RaceTitle`). During the countdown the event's name
  and lap count (stunt events: time limit) open out of a white line in the
  middle of the screen and close again as the countdown ends, instead of the
  small green name in the bottom-left corner. FINAL LAP opens and closes
  the same way, and the finish message ("Congratulations! You finished
  1st!") opens near the top of the screen over the podium cutscene.
- **Race announcer** (`Announcer`). Voice and air horn sounds play with the
  countdown, at the start, on the final lap and over the podium cutscene.
  They come in the release's `SOUNDS\` folder (`five.wav` ... `one.wav`,
  `airhorn.wav`, `finallap.wav`, `podium.wav`); replace any of them with your
  own WAV of the same name.
- **EAX reverb per event** (`BajaReverb`, `EnduroReverb`, `QuarryReverb`,
  `NationalsReverb`, `SupercrossReverb`, `TagReverb`). With EAX, outdoor
  events get Mountains (Baja, Stunt Quarry) or Forest (Enduro, Nationals)
  reverb; any EAX 1.0 preset can be chosen per event. See
  [Sound, EAX and DSOAL](#sound-eax-and-dsoal).
- **Music** (`[Music]` section, off until set). Your own WAV, FLAC or MP3
  files in the menus and races, per event or per track, in plain stereo
  without EAX or 3D effects. See [Music](#music).

### Frame pacing and smoothness
- **Fixed-rate simulation** (`SimRate=30`). The game's physics and AI were
  tuned for low frame rates and go wrong at 144 Hz. Races now always step at
  30 per second, whatever the frame rate.
- **Smooth drawing between steps** (`SimSmooth`). The camera, bikes, riders,
  shadows and name labels are drawn blended between the last two simulation
  steps, so motion is smooth at any refresh rate (one step of delay).
- **Steady controls on slow frames** (`InputTiming`). After a hitch the game
  runs several steps at once; each now gets the keys and stick as they were
  during its own slice of time, and quick taps always count for one step.
- **Precise timers** (`PreciseTimer`, `FastCrt`). The game's clocks use
  `QueryPerformanceCounter` instead of whole milliseconds.
- **Frame-rate independence** for the steps the game applies once per frame
  outside races (`FrameFix`, `RefRate`), and for AI re-planning (`AiRate`).

### Performance
- 3D sound listener updates (which hold DirectSound's lock for milliseconds
  on Windows 10/11) run on a worker thread while the frame waits for VSync
  (`AsyncSound`).
- The game's case-insensitive name compares use a lookup table instead of
  the slow CRT locale path (`FastCrt`).
- A few of the game's own loops that its 1999 compiler built badly are
  rewritten in place with identical maths (`FastGameCode`): so far the terrain
  texture coordinate loop, about 13x faster.
- The game's per-frame terrain detail code is rewritten with bit-for-bit
  identical results (`FastTerrain`), checked against the game's own code in
  an x86 emulator and optionally in the game (`TerrainCheck`); its
  float-to-integer conversion is one SSE3 instruction (`FastFtol`).
- MCM2MaX's own per-frame work uses SSE2 when the CPU has it (`Simd`):
  converting the game's vertices for the GPU, blending objects between
  simulation steps, 16-bit texture uploads, the pause menu's copy of the
  frame and texture edge clean-up. The CPU is checked at start-up, so CPUs
  without SSE2 (Pentium II/III, early Athlons) use the plain code. The game's
  own code, physics included, is unchanged.
- **No freezes on tree-heavy maps** (`FastSort`, `TreeCacheMB`). The game's
  old qsort went quadratic sorting its tree cache, freezing for up to 125 ms
  while turning; it now uses a merge sort and trims that cache far less often.
  Because the merge sort is stable, it also stops distant riders' engine
  sounds from retriggering as quiet looping snippets.
- Direct3D 11 state caching (`StateCache`), batched GPU text and cheaper
  texture uploads.
- Draw batching: runs of 3D draws with nothing changed between them (trees,
  terrain, scenery) go to the graphics card as one draw, about three times
  fewer per frame (`DrawBatch`). Still scenery skips the smoothing blend, and
  the race HUD text no longer asks Windows for font details every frame.
- **No power-plan stutter** (`RacePower`). On the Balanced power plan
  Windows slows the CPU down while the game waits for VSync, and some frames
  then run late. While racing, MCM2MaX switches to the High performance plan
  and puts yours back afterwards (not on battery).
- **Long view distances run faster**: distant trees are drawn by the
  graphics card in one go (`FarTrees`, `FarTreeCells`), the game's object
  walks share one list (`FastObjectWalk`), still scenery skips the smoothing
  checks (`StillScenery`), and the bike reflections leave out far scenery and
  update at most 60 times a second (`ReflectMaxRange`, `ReflectFaceRate`).
- A built-in profiler for finding hitches (`Profile`).

### Other
- **Xbox controllers**: XInput controllers show up under their own names,
  with analog sticks, separate trigger axes and rumble
  (`XInput`).
- **Rebuilt UI**: Original UI art and functionality cloned and made moddable
  using [RmlUi](https://github.com/mikke89/RmlUi) for easy UI modification
  and expansion.
- **Easier Easy**: on Easy, computer riders accelerate less hard and have a
  slightly lower top speed than in the original, where Easy was barely easier
  than Normal. Tunable per difficulty (`AiAccel*`, `AiTopSpeed*`).
- **In-game Help** opens `MCM2HELP.html` in your web browser.
- **Renderer label**: "Direct3D 11" or "Direct3D 9" in the bottom right
  corner of the menus (`RendererLabel`).
- **Version check**: MCM2MaX only patches the supported `Mcm2.exe`. With any
  other exe it does nothing and says so in the log.

## Requirements

- **The game files** installed in a folder, with this `Mcm2.exe`:
  - size **1,577,005 bytes**
  - SHA-256 `345e4f644fe2237108472acbcde3123b186d2475dd5a23480321c78084746f9e`

  This is the widely available no-CD exe. The original CD `Mcm2.exe` uses
  SafeDisc copy protection, which Windows 10 and 11 cannot run at all.
- **Windows 10 or 11** with a Direct3D 11 graphics card, or Windows 7/8.
  Windows XP SP3 also works; see [Windows XP](#windows-xp).
- **A Pentium II / Pentium Pro, AMD Athlon / Duron or newer CPU** (any CPU
  with the CMOV instruction). The original Pentium, Pentium MMX and AMD K6
  series (K6-2, K6-III) are not supported; on machines that old, run the
  original game. No MMX or SSE is needed, but SSE will be used if available.
- Nothing else. You don't need the DirectX 7 runtime, `d3drm.dll` from an
  old Windows, the Indeo codec or compatibility settings. If you set
  compatibility modes on `Mcm2.exe` earlier, it's best to remove them.

## Not yet tested

These work in principle but have had little or no testing so far. Reports
are welcome.

- **Windows XP support**: barely tested, it's on the TODO list. The XP support is really only there for my own personal XP gaming rig, which is very powerful (for an XP machine) and probably doesn't represent a typical XP gaming rig, so YMMV.
- **4:3, 16:10, 21:9, 32:9 and 64:27 aspect ratios** with the new HUD, text
  and other screen layout changes: not tested. Testing so far has been at
  16:9.

## Future work

- [IN PROGRESS] **Extend menu system**: More flexibility, more controls while retaining the original design aesthetic.
- **More performance**: Squeeze every last drop. Move all objects to GPU where possible.
- **Audio improvements**: Unified, low-latency audio subsystem, replacing DS7 and our current hacks.
- **Steamdeck optimisations**: Ensure it's playable and performant on Valve's handheld.
- **Updated network multiplayer support**: Not sure what form this will take right now.

## Hypothetical future work

- **Built-in track editor**: Pretty much the holy grail.
- **Split-screen multiplayer**: Needs checked for feasibility.
- **3D skyboxes**: Moving clouds, weather effects, etc. Save this one for if I get really bored.

## Installing

1. Get the release zip (`MCM2MaX-release` from the
   [GitHub Actions builds](../../actions), or a release package).
2. Copy everything in it into the game folder, next to `Mcm2.exe`:

   | File | What it is |
   |---|---|
   | `d3drm.dll` | MCM2MaX itself |
   | `MCM2MaX.ini` | settings, each one explained, all at their defaults |
   | `MCM2HELP.html` | the converted in-game help |
   | `FONTS\` | the HUD fonts (Contrail One, DSEG7 Classic) |
   | `SOUNDS\` | the race announcer sounds |
   | `UI\MAINMENU\01_MAIN\BackGround_01.png` | the main menu picture shown over the waving flag (`MenuFlag`); a new file, none of the game's are replaced |
   | `README.txt` | short player notes |
   | `README.md` | this document |
   | `licenses\` | FFmpeg, SDL, Wuffs and font licences |

3. Double-click `Mcm2.exe`.

The game loads a library called `d3drm.dll`, and Windows looks in the game
folder first, so the fix is picked up with the exe left untouched.

`MCM2MaX.ini` is optional. A missing file or a missing setting uses the
default. The game folder can be read-only (for example under
`Program Files`), because nothing is written there.

**On first run** MCM2MaX copies any existing settings from the old
`HKLM\Software\Microsoft\Microsoft Games\Motocross Madness 2` key (an
earlier install) into its own per-user key, and sets the install path from
the folder `Mcm2.exe` is in.

**Upgrading from mcm2fix** (this project's old name): on first run MCM2MaX
copies `HKCU\Software\mcm2fix` to `HKCU\Software\MCM2MaX`, so your game
settings carry over; profiles and saves are in the same per-user folder as
before. `mcm2max.ini` replaces `mcm2fix.ini`: copy any settings you changed
across, then delete `mcm2fix.ini`. Until `mcm2max.ini` is there, the old
`mcm2fix.ini` is still read, and so is a `mcm2fix-menu.ini` in the per-user
folder until `MCM2MaX-menu.ini` exists.

*Alternative for developers:* `python tools/mcm2patch.py Mcm2.exe
Mcm2_fixed.exe` makes a copy of the exe that imports `MCM2MaX.dll` by that
name instead of `d3drm.dll`. Then put `MCM2MaX.dll` next to it.

## Where your settings, saves and logs go

| What | Where |
|---|---|
| Game settings (options, controls, resolution, last profile) | `HKEY_CURRENT_USER\Software\MCM2MaX\Motocross Madness 2` |
| Profiles, replays, ghosts, user tracks, circuits, the game's own .ini and log files | `%LOCALAPPDATA%\Motocross Madness 2\` |
| MCM2MaX logs (one per run, newest 50 kept) | `%LOCALAPPDATA%\Motocross Madness 2\logs\MCM2MaX-YYYYMMDD-HHMMSS.log` |
| Dumped textures (`TextureDump=1`) | `%LOCALAPPDATA%\Motocross Madness 2\textures_dump\` |
| Your texture replacements | `textures\` in the game folder, or `%LOCALAPPDATA%\Motocross Madness 2\textures\` |
| Profiler output (`Profile=1`) | `%LOCALAPPDATA%\Motocross Madness 2\logs\perf-*.csv`, `perf-samples-*.txt` |

The per-user folder **overlays** the game folder. Any file the game writes
under its own folder goes to the same relative path under
`%LOCALAPPDATA%\Motocross Madness 2`. Reads use the per-user copy if there is
one, otherwise the game folder's. Folder listings show both, so stock
tracks and your own appear together. Nothing is written to the game folder
or to `HKLM`.

On Windows XP, `%LOCALAPPDATA%` means
`C:\Documents and Settings\<user>\Local Settings\Application Data`.

## Uninstalling

Delete `d3drm.dll`, `MCM2MaX.ini`, `MCM2HELP.html`, `FONTS\`, `README.txt`,
`README.md`, `licenses\`, `SOUNDS\` and `UI\MAINMENU\01_MAIN\BackGround_01.png` from the game folder. To also remove your settings and saves,
delete `HKCU\Software\MCM2MaX` and `%LOCALAPPDATA%\Motocross Madness 2`.

## Renderers and fullscreen

| `Renderer=` | What it does |
|---|---|
| `D3D11` (default) | Direct3D 11 on the high-performance GPU, with a flip-model swap chain. It uses one shader that reproduces Direct3D 7's fixed-function pipeline (two texture stages, fog, alpha test, specular, colour key). |
| `D3D9` | A fixed-function Direct3D 9 renderer with no shaders. It's used automatically when Direct3D 11 isn't available (Windows XP, very old GPUs), and you can choose it yourself on any system. |

`d3d11.dll`, `dxgi.dll` and `d3d9.dll` are loaded at run time, so the DLL
loads even where one of them is missing. Both renderers support MSAA,
anisotropic filtering, GPU text, texture replacement and exclusive
fullscreen.

| `Fullscreen=` | What it does |
|---|---|
| `Borderless` (default) | A window that covers the primary monitor at desktop resolution. Alt+Tab and second monitors behave like any window. |
| `Exclusive` | The game owns the screen, at the desktop mode or `ExclusiveResolution=<w>x<h>[@<Hz>]` (pick it in the game's Options). The game's picture is still scaled into it. Alt+Tab leaves fullscreen, and fullscreen comes back when you return to the game. This can help with VSync on Windows XP. |

The resolution you pick in the game's options is the size the game renders
at. Borderless or exclusive, the picture is scaled to fill the screen with
black bars only where the aspect ratios differ.

## Windows XP

The same `d3drm.dll` loads on Windows XP SP3 (tested on real hardware). It is
built with MinGW, and CI checks that it imports nothing XP lacks.

- XP has no Direct3D 11, so the Direct3D 9 renderer is used. Install the
  DirectX 9.0c runtime if it is missing.
- Saves and logs go to `Local Settings\Application Data\Motocross Madness 2`.
- Without a desktop compositor, windowed Direct3D 9 often ignores VSync, so
  MCM2MaX waits for the vertical blank itself (`VSync=1`).
  `Fullscreen=Exclusive` avoids the issue entirely.

Details: [docs/windows-xp.md](docs/windows-xp.md).

## Sound, EAX and DSOAL

The game uses DirectSound 3D. On Windows 10/11, DirectSound is emulated in
software and reports no hardware 3D voices, so the game's **EAX** reverb
never turns on.

[DSOAL](https://github.com/kcat/dsoal) (DirectSound on OpenAL Soft) brings
back hardware-style 3D sound and EAX. Put its **32-bit (Win32)** DLLs in the
game folder. The 64-bit ones are silently ignored by the 32-bit game, which
then falls back to Windows' own DirectSound. With DSOAL the game finds EAX at
startup and applies its reverb: Arena in Supercross and Moto-Tag. MCM2MaX
works with or without DSOAL.

For every outdoor event the game asks for "reverb off" with a decay time of
0, which OpenAL Soft rejects, so the reverb from before stayed on instead.
MCM2MaX sends a real outdoor preset instead: Mountains for Baja and Stunt
Quarry, Forest for Enduro and Nationals. `BajaReverb`, `EnduroReverb`,
`QuarryReverb` and `NationalsReverb` take any EAX 1.0 preset name, or
`Default` for the game's own values. `SupercrossReverb` and `TagReverb` can
replace the game's Arena reverb in Supercross and Moto-Tag too; they are set
to `Default`, which keeps the game's Arena.

DSOAL ignores the `DSBCAPS_MUTE3DATMAXDISTANCE` flag that the game sets on
all its 3D sounds, so riders who are out of earshot can still be heard
quietly. `SoundMaxDistanceMute=1` (the default) has
MCM2MaX mute 3D sounds beyond their max distance itself, as Windows'
DirectSound does, when the game is using a replacement DirectSound. With
Windows' own DirectSound MCM2MaX leaves this to Windows: muting on top of it
made the bike engines go silent after every pause. The game's own sounds fade
to -20 dB at max distance and then cut off, so a slight snap-off at the edge
is original behaviour.

The quiet looping snippets of old sounds from distant riders, which first
looked like a DSOAL problem, came from the game itself and happen with any
DirectSound. The game re-sorts its engine voices on every update with the C
library's qsort, which doesn't keep equal entries in order, so voices were
handed between riders and restarted. `FastSort=1` (the default) replaces
that sort with a stable one and stops it.

## Music

The game has no music of its own (only an unused CD audio player), so
MCM2MaX can play yours: `.wav`, `.flac` or `.mp3` files, set in the
`[Music]` section of `MCM2MaX.ini`. Nothing plays until you set something.

```ini
[Music]
Menu=MUSIC\menu.mp3
Race=MUSIC\race
Supercross=MUSIC\stadium1.flac;MUSIC\stadium2.flac
BAJA01=D:\Music\desert.mp3
```

- `Menu` plays in the menus.
- In a race, the track's own key is used if it has one (the track's file
  name in the game's `Teraform` folder without `.env`: `BAJA01`, `SX05`,
  `NAT12`, `urbanjunglebaja` and so on), else its event's key (`Baja`,
  `Enduro`, `Nationals`, `Quarry`, `Supercross`, `Tag`), else `Race`. If none
  is set, the race is silent.
- A value is a file or a folder (every music file in it), relative to the
  game folder or a full path. Separate several with `;`. A list plays in
  turn and repeats (`Shuffle=1`: in random order); a single file loops
  without a gap. The game's install has an empty `MUSIC` folder, which is a
  good place for them. A relative path is also looked for in
  `%LOCALAPPDATA%\Motocross Madness 2`, for an install you can't write to.
- Menu music starts when the menus appear, not during the intro slides and
  video. It fades out quickly as a track starts loading, and the race music
  starts with the countdown. When you finish, the race music fades out
  quickly and stays off through the podium cutscene; the menu music plays on
  the results. Other changes fade over `FadeTime`.
- Race music pauses with the race's pause menu (`PauseInRace`), all music
  pauses while the game is in the background (`BackgroundMute`), and nothing
  plays while one of the game's videos with sound (the intro) does; the
  menus' silent animations don't count.
- `MusicVolume` and `RaceMusicVolume` only set the music. The game's own sound volume
  and `AnnouncerVolume` don't change it; Windows' volume mixer entry for the
  game turns everything down together.
- Music is played through Windows' plain audio output (waveOut), not
  DirectSound, so the game's 3D positioning and EAX reverb (DSOAL included)
  never touch it. It works the same on Windows XP.
- The music keys are read again at each race and each return to the menus,
  so you can change them without restarting the game, as long as some music
  was set when it started.

## MCM2MaX.ini reference

`MCM2MaX.ini` sits next to `Mcm2.exe`. Its settings go in a section called
`[MCM2MaX]`, apart from the music settings, which go in `[Music]`. Every setting is optional, and a missing one (or a missing file)
uses the default below. On/off settings take `1` or `0`. Lines starting with
`;` are comments. Changes take effect the next time the game starts. The
shipped file explains every setting in its comments, in the same order as
these tables.

The file has three parts:

1. **[Preferences](#1-preferences)**: change these to taste.
2. **[Advanced](#2-advanced-changing-these-is-not-recommended)**: these keep
   the game starting, saving and playing correctly. Changing them is **not
   recommended**.
3. **[Logging](#3-logging)**: only the main log is on. The rest are for
   debugging and are all off in the release.
4. **[Music](#4-music-music-section)**: the `[Music]` section, empty (off)
   in the release.

### 1. Preferences

#### Display

| Setting | Default | Values | Description |
|---|---|---|---|
| `Renderer` | `D3D11` | `D3D11`, `D3D9` | The graphics API. D3D9 is a fixed-function Direct3D 9 renderer, and it's used automatically when Direct3D 11 is missing (Windows XP, very old cards). |
| `Fullscreen` | `Borderless` | `Borderless`, `Exclusive` | *Borderless*: a borderless window covering the monitor at its current resolution. The desktop resolution never changes and Alt+Tab is instant. *Exclusive*: the game takes over the screen at `ExclusiveResolution`, Alt+Tab leaves fullscreen and it comes back when you return. This can help VSync on XP. Each has its own resolution, and the game's Options list the monitor's own resolutions for the current one (Exclusive: with refresh rates). |
| `Monitor` | `1` | `1`, `2`, ... | The monitor to use: `1` = primary, `2` and up = the other monitors in Windows' order. |
| `BorderlessResolution` | `Desktop` | `Desktop`, `<w>x<h>` | The resolution the game renders at in Borderless, scaled to the monitor with black bars where the shape differs. Picking one in the game's Options writes it here. Up to the desktop's size; a size that's no longer available uses the desktop for that start. |
| `ExclusiveResolution` | `Desktop` | `Desktop`, `<w>x<h>`, `<w>x<h>@<Hz>` | The screen mode in Exclusive. Picking one in the game's Options writes it here and (Direct3D 11) switches the screen at once, then asks *Keep this display mode?*: Keep (Enter) or Revert (Esc), and with no answer in 10 seconds the previous mode comes back. Without `@<Hz>`, the highest refresh rate is used. A mode the monitor doesn't have uses the desktop for that start. The menus stay at this mode too, scaled. |
| `CardTest` | `0` | `0`, `1` | `0` skips the game's video card test at start (MCM2MaX's renderer always gives the same answers, so the card gets them directly). `1` runs the game's own test, as before. |
| `VSync` | `1` | `0`, `1` | `1` waits for the monitor's refresh before showing each frame: no tearing, and the frame rate is limited to the refresh rate. `0` shows frames as soon as they are ready, with tearing where Windows allows it. |
| `FrameCap` | `500` | `0`, frames/s | The most frames per second, on top of VSync. `0` = no cap. It matters with `VSync=0`. It does not change game speed. |
| `MenuFpsCap` | `240` | `0`, frames/s | The most frames per second outside races (menus, rider and bike select, loading, the pause menu), even with `VSync=0`. Races, including the intro, countdown and podium cutscene, are not affected. `0` = no menu cap. If `FrameCap` is lower, that wins. |
| `WideFov` | `1` | `0`, `1` | `1` widens the view on widescreen resolutions, so you see more to the sides. `0` keeps the 4:3 width and crops the top and bottom, like the original. |
| `Fov` | `0` | `0`, 20-150 | Field of view in degrees, as a 4:3 screen would show it. `0` = the game's default (77). `WideFov` widens it further on widescreen, and the zoom keys still step from it. |
| `RendererLabel` | `1` | `0`, `1` | Shows "Direct3D 11" or "Direct3D 9" in the bottom right corner of the menus, only while no 3D scene is being drawn. |

#### Graphics quality

| Setting | Default | Values | Description |
|---|---|---|---|
| `Antialiasing` | `4` | `0`/`1` (off), `2`, `4`, `8` | Multisample antialiasing (MSAA) of the 3D scene. If the card can't do the number asked for, the next lower one is used. |
| `Anisotropy` | `16` | `0`/`1` (off), 2-16 | Anisotropic filtering, which keeps the ground sharp at shallow angles. It's capped at what the card supports. |
| `MipBias` | `0` | -4 to 4 | Texture sharpness at a distance. Negative values are sharper but may shimmer (e.g. `-0.5`); positive values are softer. |
| `ViewDistance` | `1` | 0.25-10 | Multiplies how far the game draws. The far clip, the distance objects such as trees are drawn to, and the fog move out together, so the fog still hides the edge. Higher values cost frame rate. |
| `FogDistance` | `1` | 0.1-20 | Multiplies the fog distances on top of `ViewDistance` (`2` = fog starts twice as far away). Above 1, the edge of the drawn world may show unless `ViewDistance` is raised too. |
| `TerrainHoleFix` | `1` | `0`, `1` | Fixes holes in distant terrain (sky through hills, trees standing on nothing) that come and go as the camera turns, mostly with a high `ViewDistance`: the game forgot to refresh the smallest terrain pieces of a block that was wholly in view. `0` = the game's own behaviour. |
| `TerrainDetail` | `2` | 0.25-8 | How finely the terrain is built (`1` is the game's own). Above 1, hills keep more of their shape further away and change shape less as you ride towards them, at some cost in frame rate; below 1, less. |
| `TerrainLodFix` | `1` | `0`, `1` | One of the game's terrain detail numbers stops at its top value (65535) instead of wrapping round to a small one, as its other ones already do. It has not been seen to reach the top so far, even with `ViewDistance=10`. |
| `ModelLod` | `1` | 0.25-10 | Detailed models stay this many times further away before the game switches to simpler versions. |
| `TreeDetailDistance` | `1` | `0`, `1` | `0` makes every tree use its sparse close-up version at any distance. It looks sparse and is for fun only. |
| `AlphaCut` | `128` | 0-255 | For trees, flags and other cut-out pictures, pixels less opaque than this aren't drawn. This removes the stair-step fringe where tree edges hide what's behind them, a flaw of the original. `0` = the original look. Soft see-through layers (shadows, ground detail, supercross ground) are never affected. With `AlphaToCoverage` on, this is where the smoothed edge sits instead. |
| `AlphaToCoverage` | `2` | `0`, `1`, `2` | Smooth, antialiased edges on cut-out pictures (tree leaves, flags, fences, metal arches, tents, desert plants) by turning their transparency into MSAA coverage. Only works while `Antialiasing` is 2 or more. `2` = soft: leaves and flags use their transparency as is, so distant trees look fuller and fade smoothly between their far-away and close-up versions; edges are softer (fences and solid models are always sharpened). `1` = sharpened: crisp edges about one pixel wide, but trees switch abruptly between versions. `0` = off. Direct3D 11 only; the Direct3D 9 renderer uses `AlphaCut`. |
| `Reflections` | `2` | `0`, `1`, `2` | A soft reflection on the bikes' bodywork, the riders' helmets and, more faintly, their suits. `1` = the track's sky (cheap: redrawn only when a new piece of sky comes into view). `2` = live: the sky plus the terrain, trees and track objects around the player's bike, redrawn from the bike's position up to `ReflectFaceRate` times a second. Behind the camera it shows what was drawn there recently (`ReflectCache`). Direct3D 11 only. |
| `ReflectBike` / `ReflectHelmet` / `ReflectRider` | `1` / `1` / `0.25` | 0-1 | How strongly bike bodywork, helmets and the rider's suit, gloves and boots reflect, seen edge-on. `ReflectStrength` (default `1`) multiplies all three. |
| `ReflectBikeFresnel` / `ReflectHelmetFresnel` / `ReflectRiderFresnel` | `0.25` / `0.25` / `0.1` | 0-1 | Share of the strength seen when you look straight at the surface; real paint and plastic shine mostly at the edges, higher values look more like chrome. |
| `ReflectBikeBlur` / `ReflectHelmetBlur` / `ReflectRiderBlur` | `4` / `4` / `4` | 0-8 | How soft the reflection is; `0` = mirror-sharp. Small curved parts like the helmet squeeze the whole sky into a few pixels, so low values show a hard horizon line there. |
| `ReflectBikeSun` / `ReflectHelmetSun` / `ReflectRiderSun` | `0.6` / `0.7` / `0.15` | 0-4 | Brightness of the sun's highlight (the light that sets the riders' shadows) on bodywork, helmets and suits. `ReflectSun` (default `1`, `0` = none) multiplies all three. Tracks without a sun have none. |
| `ReflectBikeShine` / `ReflectHelmetShine` / `ReflectRiderShine` | `40` / `40` / `8` | 1-2000 | How small and sharp the sun's highlight is; higher looks glossier. |
| `ReflectSize` | `128` | 16-1024 | Reflection cube side in pixels. |
| `ReflectLiveFaces` | `6` | 1-6 | With `Reflections=2`, how many of the reflection's six cube sides each update redraws (`6` = all of it; fewer spreads the work over several updates). |
| `ReflectCache` | `1` | `0`, `1` | With `Reflections=2`, terrain, trees and track objects drawn in recent frames are kept on the graphics card, so the reflection also shows what is around the bike behind the camera, not just sky. `ReflectCacheMB` (default `128`) is its memory (with a long view distance, less forgets things soon after they leave view), `ReflectCacheSecs` (default `20`) how long something no longer drawn is kept, `ReflectCacheRadius` (default `0` = as far as the fog ends) how far from the bike kept things are reflected. |
| `ReflectMaxRange` | `1024` | `0`, world units | The reflection leaves out terrain and scenery further than this from the bike, whatever `ViewDistance` is (`1024` is a little past the fog at `ViewDistance=1`; `0` = no limit). Far scenery is too small to see in the reflection, and with a long view distance drawing it all again costs frame rate. |
| `ReflectFaceRate` | `60` | `0`, updates/s | How many times a second, at most, the reflection is updated; each update redraws `ReflectLiveFaces` sides. Frames between updates skip the work. `0` = every frame. |
| `ReflectGround` | `1` | 0-2 | Brightness of the ground below the horizon in the reflection, as a share of the fog colour. |
| `ReflectBikeTextures` / `ReflectHelmetFrames` / `ReflectRiderFrames` / `ReflectExclude` / `ReflectTextures` / `ReflectFrames` | see ini | names | Which parts reflect: bike textures by name pattern (`*` matches anything), model parts by frame name (the rider's `Head` is the helmet; `ReflectRiderFrames` lists the body parts), textures that never reflect (`tire*`), and extra `name:strength` entries. |
| `ReflectSkipObjects` | `SIGN*, FLAG*` | frame names | Models with any of these parts get no reflections: the board man at the start line (`SIGN01`) and the flag man (`FLAG01`-`FLAG03`) are the rider model with extra parts, so their bare heads would shine like helmets. |
| `LiquidMetal` | `0` | `0`, `1` | "T-1000 mode": every bike and rider is drawn in liquid chrome, reflecting the world around it (uses `Reflections=2` if `Reflections` is `0`). |
| `MotionBlur` | `1` | `0`, `1` | Speed blur: above `MotionBlurMinSpeed` the edges of the 3D scene streak towards the point the bike is heading for, more the faster you go, while the middle of the screen stays sharp. The HUD, text and menus are never blurred. Works the same at any frame rate. |
| `MenuOptions` | `1` | `0`, `1` | MCM2MaX settings on the game's own Options pages: Options > Graphics gets an "Enhancements" box with sliders for `Anisotropy`, `Antialiasing` and `ViewDistance` (1x to 10x) and checkboxes for `VSync`, `WideFov` ("Widescreen View"), `MotionBlur`, `AlphaToCoverage` ("Smooth Tree Edges", greyed while Antialiasing is off), `RiderShadows`, `Reflections` and `TextureReplace` ("Replacement Textures"; `MenuBackground` is always used). Antialiasing, View Distance, Widescreen View, Soft Rider Shadows, Bike Reflections and Replacement Textures apply the next time the game starts. Choices made there are written into `MCM2MaX.ini`; if it can't be written they go to `%LOCALAPPDATA%\Motocross Madness 2\MCM2MaX-menu.ini`, which then wins over `MCM2MaX.ini` (delete it to go back). |
| `MenuHiRes3D` | `1` | `0`, `1` | 3D in the menus (the bike and rider in the garage on the Bike & Rider page) is drawn at the window's resolution instead of 640x480 scaled up. Needs the Direct3D 11 renderer and `GpuText=1`. |
| `MenuMaX` | `1` | `0`, `1` | New menu screens made from text files in `UI\MaX\` with [RmlUi](https://github.com/mikke89/RmlUi) (HTML/CSS-style `.rml` / `.rcss`, PNG pictures, TTF fonts), drawn at the window's resolution over the game's menus. Every screen works with the mouse, the keyboard (arrows / Tab move, Enter selects, Left / Right change sliders and lists, Esc goes back) and controllers (d-pad / stick, A, B). Controls save `mcm2max.ini` settings with no code (`ini="Key"`, see `UI\MaX\README.txt`), and a screen reloads as soon as its files are saved. For now: **More Settings** on Options > Graphics. Works with both renderers (Direct3D 11, and Direct3D 9 for `Renderer=D3D9` and Windows XP). |
| `MaXMenus` | `1` | `0`, `1` | The game's menus drawn by MenuMaX (`UI\MaX\menus\`): copies of the game's own screens, sharp at any resolution and usable with the mouse, keyboard and controllers. They work through the game's own menus underneath (which still do all the saving), and their pictures are read from the game's own `UI\*.DAT` files (a PNG under `UI\MaX\dat\` replaces one). Copies so far: the main menu, Options (all pages), single player, multiplayer, career, high scores, replays and the popups; any screen without a copy shows the game's own. Needs `MenuMaX=1`. |
| `MenuNav` | `1` | `0`, `1` | The game's own menu screens (main menu, Options, event screens, ...) also work with the keyboard and controllers, and look exactly as they did: arrows / d-pad / left stick (a wheel's or joystick's hat or stick) move the menu cursor to the nearest button (or list row) in that direction, where it lights up as under the mouse; when a pull-down panel opens, the cursor goes to its first button; Enter / A (joystick button 1) clicks it; Esc / B (button 2) clicks the screen's Back or Cancel. Keys a screen uses itself (Enter / Esc on message boxes, the replay player's arrows) stay the game's. The mouse works as before. Needs `InputTiming=1`. |
| `ZoneFix` | `1` | `0`, `1` | The Multiplayer panel's MSN Gaming Zone button is greyed out and does nothing: the Zone closed long ago, and clicking it crashed or closed the game. |
| `MenuFlag` | `1` | `0`, `1` | A waving dark blue chequered flag behind the main menu, seen through the transparent parts of `MenuBackground` (fills the whole window). Needs that picture and the Direct3D 11 renderer; otherwise the main menu is as it was. |
| `MenuFlagSpeed` | `100` | `0`-`1000` | How fast the menu flag moves, in percent (`0` = still). |
| `MenuFlagBrightness` | `68` | `0`-`300` | Brightness of the menu flag, in percent. |
| `MenuFlagWarp` | `100` | `0`-`500` | How far the flag's chequers are pushed around, in percent (`0` = straight lines). |
| `MenuFlagShading` | `100` | `0`-`300` | Strength of the flag's light, shadow and drifting patches, in percent (`0` = flat). |
| `MenuBackground` | `UI\MAINMENU\01_MAIN\BackGround_01.png` | path | The main menu background used with `MenuFlag`: a 640x480 PNG with transparency, relative to the game folder or a full path. The release ships one at that path. `tools/mcm2dat` unpacks the original to start from. |
| `MotionBlurStrength` | `80` | 0-300 | Length of the streaks at full speed, in percent (higher = longer streaks). |
| `MotionBlurMinSpeed` | `30` | 0-200 | Speed (mph, as the speedometer shows) where the blur starts. |
| `MotionBlurMaxSpeed` | `70` | 1-250 | Speed (mph) where the blur is at full strength. |
| `MotionBlurCenter` | `60` | 0-95 | Size of the sharp middle of the screen, in percent of the way from the middle to the edges. Only the part outside it streaks, most at the edges. |
| `TrueColorTextures` | `1` | `0`, `1` | Loads textures in full colour (24-bit, or 32-bit with alpha) wherever the picture has more than 16 bits; a bug made the game always use 16-bit. Most of the game's art is stored in 16-bit, so only a few textures change. Changing it renames unnamed dumped textures. |
| `TextureReplace` | `1` | `0`, `1` | Uses replacement pictures from `textures\` in the game folder or in `%LOCALAPPDATA%\Motocross Madness 2\textures\` (the per-user folder wins). See [Texture dumping and replacement](#texture-dumping-and-replacement). |
| `TextureDump` | `0` | `0`, `1` | Saves every texture the game loads as a PNG in `%LOCALAPPDATA%\Motocross Madness 2\textures_dump`, to start replacements from. Loading is slower while it's on. |
| `TextureDecodeThread` | `1` | `0`, `1` | Reads replacement pictures on a separate thread, so a big picture never holds up the game; the game's own texture shows until the picture is ready, a few frames later. `0` reads each picture when the game loads the texture. |
| `TexturePreload` | `1` | `0`, `1` | Reads every replacement picture in the textures folders while the game starts up (on the decoding thread, then a few each frame onto the graphics card), so a replacement is already there when the game first loads its texture instead of showing the original for a few frames. `TexturePreloadMB` (default `256`) caps the picture memory read this way; the rest load when needed. Needs `TextureDecodeThread=1`. |
| `RiderShadows` | `2` | `0`, `1`, `2` | `2`: every rider and bike casts a shadow, drawn by the graphics card at high resolution with soft edges, following the riders smoothly between simulation steps. `1`: only the rider the camera follows has one, as in the original game, but drawn the same new way. `0`: the game's own shadow (one rider, drawn into a small picture on the CPU). Direct3D 11 only; the Direct3D 9 renderer always uses the game's shadow. Tracks where the game draws no shadow (no sun) still have none. |
| `ShadowStrength` | `0` | `0`, 1-100 | How dark the `RiderShadows` shadows are, in percent. `0` = as dark as the game's own shadow. |
| `ShadowSoftness` | `100` | 0-400 | How much the `RiderShadows` shadows blur and fade with the rider's height above the ground, in percent. Crisp where the wheels touch the ground, wide and faint under a high jump. `0` = always sharp. |

#### Text and HUD

| Setting | Default | Values | Description |
|---|---|---|---|
| `GpuText` | `1` | `0`, `1` | The game's menu and HUD text is drawn by the graphics card, antialiased, at screen resolution. `0` = the game's own text, drawn at the game's resolution and scaled up. |
| `HudText` | `1` | `0`, `1` | Redraws the race HUD panels (position, lap, times, stunt score) and the rider name labels sharply, using the sizes and fonts below. Needs `GpuText=1`. |
| `HudScale` | `2` | 1-4 | Size of the race HUD text panels (`1` = original). |
| `LabelScale` | `1.5` | 1-4 | Size of the name labels over the riders (`1` = original). Names that would be too long are shrunk to fit. |
| `LineWidth` | `3` | 1-16 | Width in pixels of the speedometer needle, antialiased (`1` = original thin line). |
| `HudFont` | `FONTS\ContrailOne-Regular.ttf` | .ttf path | Font for the HUD panel and name label text, relative to the game folder or a full path. It's used by the game only and isn't installed in Windows. Empty = the game's fonts. |
| `HudDigitFont` | `FONTS\DSEG7Classic-BoldItalic.ttf` | .ttf path | Font for the numbers in the race stats panel (times, position, lap); words keep `HudFont`. Characters it lacks fall back to `HudFont`. Empty = `HudFont`. |
| `HudDigitSize` | `0.7` | 0.5-2 | Size of those numbers relative to the words beside them. |
| `HudStuntTextSize` | `0.8` | 0.5-1.5 | Stunt events: size of the score panel's text relative to the race panel's. |
| `RaceTitle` | `1` | `0`, `1` | During the countdown, the event's name and lap count (stunt events: time limit) open out of a white line in the middle of the screen and close back into it as the countdown ends, replacing the game's small green name in the bottom-left corner. FINAL LAP is shown the same way (held 2 seconds) instead of dropping down the screen, and the finish message opens near the top of the screen for the length of the podium cutscene (top 3; otherwise the game shows it as usual). Needs `GpuText=1`. `0` = the game's corner text and drop text. |
| `RaceTitleSize` | `1` | 0.5-2 | Size of that title. |
| `Announcer` | `1` | `0`, `1` | Plays the WAV files in `SOUNDS\` in the game folder (the release includes all eight; replace any with your own of the same name): `five.wav` ... `one.wav` as the countdown passes 5 ... 1 seconds left, `airhorn.wav` when the race starts, `finallap.wav` when FINAL LAP shows (not in one-lap races, where it would play over the start) and `podium.wav` when the podium cutscene starts. Missing files are skipped. 8 or 16-bit PCM. |
| `AnnouncerVolume` | `100` | 0-100 | Volume of those sounds, in percent. |
| `BajaReverb` | `Mountains` | preset name, `Default` | EAX reverb in Baja events. The presets are the 26 EAX 1.0 environments: `Generic`, `PaddedCell`, `Room`, `Bathroom`, `LivingRoom`, `StoneRoom`, `Auditorium`, `ConcertHall`, `Cave`, `Arena`, `Hangar`, `CarpetedHallway`, `Hallway`, `StoneCorridor`, `Alley`, `Forest`, `City`, `Mountains`, `Quarry`, `Plain`, `ParkingLot`, `SewerPipe`, `Underwater`, `Drugged`, `Dizzy`, `Psychotic`. `Default` = the game's own values. Applies to every track raced as that event, add-on tracks included. Needs EAX (DSOAL or an EAX sound card); otherwise it changes nothing. |
| `EnduroReverb` | `Forest` | preset name, `Default` | The same for Enduro events. |
| `QuarryReverb` | `Mountains` | preset name, `Default` | The same for Stunt Quarry events. |
| `NationalsReverb` | `Forest` | preset name, `Default` | The same for Nationals events. |
| `SupercrossReverb` | `Default` | preset name, `Default` | The same for Supercross events. `Default` keeps the game's Arena reverb. |
| `TagReverb` | `Default` | preset name, `Default` | The same for Moto-Tag events. `Default` keeps the game's Arena reverb. |

#### Race direction arrow

| Setting | Default | Values | Description |
|---|---|---|---|
| `ArrowFix` | `1` | `0`, `1` | The green direction arrow (bottom left) points at the next gate. `0` = the original arrow, which leans toward the screen centre because of a perspective bug. |
| `ArrowTurn` | `0.8` | 0-2 | How much of that correction is applied (`1.0` = the full correction). Lower it if the arrow points left of a gate straight ahead, and raise it if it points right. |
| `ArrowTilt` | `10` | -45 to 45 | Degrees the arrow's nose is raised so it's seen a little from above (`0` = level, as in the original). |
| `ArrowScale` | `2.0` | 0.25-8 | Size of the arrow (`1.0` = original). |

#### Computer riders

| Setting | Default | Values | Description |
|---|---|---|---|
| `AiAccelEasy`, `AiAccelNormal`, `AiAccelHard` | `80`, `130`, `150` | 10-400, percent | How hard computer riders can accelerate on each difficulty, in percent of the game's base curve. Lower values make them pull away more slowly out of corners and after landings. The original game uses `110` for Easy, which is barely easier than Normal, especially in Supercross. |
| `AiTopSpeedEasy`, `AiTopSpeedNormal`, `AiTopSpeedHard` | `90`, `100`, `100` | 50-150, percent | Computer riders' top speed on each difficulty. The original game uses `100` for all three. |

#### Controllers

| Setting | Default | Values | Description |
|---|---|---|---|
| `ControllerHub` | `1` | `0`, `1` | Every gamepad (Xbox, PlayStation, Switch Pro and other pads SDL knows) drives one game controller, "Gamepad (any controller)", laid out as described under `XInput`. The pad you touched last drives it, and pads can be plugged in or out at any time. The Controls screen names buttons after the driving pad (A, Cross, LB, L1...). Windows' DirectInput copies of these pads are hidden from the game; wheels and other joysticks stay as they were. The `XInput*` settings apply to it too. `0` = one device per Xbox controller (`XInput=1`), or none. |
| `KeyboardPad` | `1` | `0`, `1` | With `ControllerHub`, the keyboard is one more controller: pressing a driving key hands driving to the keyboard, and touching a pad hands it back, so you can carry on with the keyboard if a pad is unplugged mid-race. Steering and lean ease in and out like the game's own key steering. While a stunt key is held, the throttle and brake keys also push the stick up and down for the stunt's direction. |
| `KeySteerLeft`, `KeySteerRight`, `KeyLeanForward`, `KeyLeanBack`, `KeyThrottle`, `KeyBrake`, `KeyStunt1`, `KeyStunt2` | `Left`, `Right`, `W`, `S`, `Up`, `Down`, `Z`, `X` | key names | The keys `KeyboardPad` uses (the game's own keyboard driving keys). A letter or digit, `Left` `Right` `Up` `Down`, `Space` `Enter` `Backspace` `Tab` `Shift` `Ctrl` `Alt`, `Insert` `Delete` `Home` `End` `PageUp` `PageDown`, `F1`-`F12`, `Numpad0`-`Numpad9`, or `None`. The stunt keys press buttons 3 and 4 (X and Y), the Dual Analog Game Pad preset's stunt buttons. |
| `ControllerPicker` | `0` | `0`, `1` | `1` = the game's startup box asking which controller to use (shown when it sees two or more and doesn't recognise the last one). `0` = no box: the last one used, else the first listed. |
| `DisabledControllers` | empty | list | Pads the controller hub ignores, comma separated, by USB id (`045E:0B12`) or part of the name (`Razer`). The log lists each pad's name and id. |
| `XInput` | `1` | `0`, `1` | Xbox (XInput) controllers connected when the game starts are listed under their own names (such as "Controller (Xbox One For Windows) (XInput 1)"), in place of the copies Windows makes of them for older games. The sticks are analog: left stick X and Y, right stick X Rotation and Y Rotation. The right trigger is the Z axis and the left trigger the Z Rotation axis, each reading zero when let go. The "Dual Analog Game Pad" preset puts Accelerate on the right trigger and Brake on the left trigger. The d-pad is the point-of-view hat, and A B X Y LB RB Back Start and the stick clicks are buttons 1 to 10. The game's force feedback becomes rumble. A controller unplugged mid-game reads as centred and works again when plugged back in. `0` = the original: Windows' DirectInput copy, which the game treats as a gamepad (sticks act as on/off presses), with both triggers on one axis and no rumble. Needs Windows Vista or later (or the DirectX XInput DLL). |
| `XInputRumble` | `100` | 0-200, percent | Rumble strength. `0` = no rumble, and the controller doesn't offer force feedback to the game. |
| `XInputTriggerButtons` | `1` | `0`, `1` | The triggers also work as buttons 11 (LT) and 12 (RT), so the "Dual Analog Game Pad" preset (Accelerate on the right trigger's axis, Brake on button 11) works as it is. `0` = axes only. The game can't bind a trigger's axis by pressing it while remapping (it looks for movement from the centre); use the preset for the right trigger. |
| `XInputRumbleForces` | `0` | `0`/`1` | `1` = the game's steady stick forces rumble too (a constant buzz while riding). `0` = only bumps, landings and crashes rumble. |
| `XInputRumbleThreshold` | `10` | 1-100, percent | With `XInputRumbleForces=0`, how sudden a change in the game's force has to be to rumble. Lower = more bumps felt; too low becomes a constant rumble. |
| `QuarryRumbleThreshold`, `BajaRumbleThreshold`, `NationalsRumbleThreshold`, `SupercrossRumbleThreshold`, `TagRumbleThreshold`, `EnduroRumbleThreshold` | `0` (Nationals, Supercross: `20`) | 0-100, percent | `XInputRumbleThreshold` for one event type; `0` = use `XInputRumbleThreshold`. Ridge and whoop tracks need a higher value. |
| `XInputRumbleMinMs` | `150` | 0-1000, ms | Shortest rumble: each bump is held at full strength this long before it fades (bumps fade over 0.4 s, crashes over 0.7 s). |
| `XInputSteerCurve` | `33` | 33-300, percent | Left stick steering response. The game already curves joystick steering (gentle near the centre, steep near the edge). `33` = straight line from stick to bike; `100` = the game's own curve; higher is softer near the centre and steeper at the edge. |
| `XInputSteerSmooth` | `60` | 0-500, ms | How long the steering takes to catch up with the left stick. Takes the edge off small thumb movements; too much feels sluggish. `0` = off. |
| `KeepControlPreset` | `1` | `0`/`1` | Keep the chosen control preset between runs while the same controller is used. The game itself only keeps it when its "use last controller" setting is on. |
| `XInputDeadzone` | `15` | 0-50, percent | How far either stick has to move from the centre before it counts. Raise it if the bike steers by itself. |

### 2. Advanced: changing these is not recommended

These settings keep the game starting, saving and playing correctly on modern
PCs. Changing them can stop the game from starting, lose settings or saves,
make bikes and computer riders behave differently, make motion judder, or
bring back bugs of the original game. Leave them alone unless you're
troubleshooting.

#### Compatibility

| Setting | Default | Values | Description |
|---|---|---|---|
| `Startup` | `1` | `0`, `1` | No video card picker (the primary display adapter is used) and no "detected a change in your video hardware" box. `0` = the original prompts. |
| `Registry` | `1` | `0`, `1` | The game's settings are kept in `HKCU\Software\MCM2MaX\Motocross Madness 2`; on first run, settings from an earlier install in HKLM are copied there. `0` = the original HKLM key, which needs administrator rights. |
| `Files` | `1` | `0`, `1` | Everything the game writes (profiles, replays, ghosts, tracks, circuits, its own .ini and log files) goes to `%LOCALAPPDATA%\Motocross Madness 2`, and is read from there first. `0` = the game folder, which fails under Program Files without admin rights. Saves made with `1` aren't seen with `0`. |
| `OneCore` | `0` | `0`, `1` | `1` runs the game on one CPU core. This was an old workaround for a startup hang that is now fixed, and it costs performance. |

#### Simulation and timing

| Setting | Default | Values | Description |
|---|---|---|---|
| `SimRate` | `30` | `0`, 10-1000 | During races, the physics and AI run this many steps per second whatever the frame rate. They were tuned at low frame rates and go wrong when stepped faster, so 30 is the tested value. `0` = one step per drawn frame, the original behaviour (bikes and AI misbehave at high frame rates). Menus always step once per frame. |
| `SimSmooth` | `1` | `0`, `1` | With `SimRate`, each frame shows the camera, bikes, riders, shadows and name labels blended between the last two steps. Motion is smooth at any refresh rate, with one step (1/30 s) of delay. `0` = motion judders at 30 steps a second. |
| `InputTiming` | `1` | `0`, `1` | With `SimRate`, each race step gets the controls as they were during its own 1/30 s, also when a slow frame makes several steps run at once, and a key or button tapped faster than one step still counts for one step. `0` = the original reading: after a slow frame every catch-up step gets the controls as they are at that moment, and quick taps can be missed. |
| `PreciseTimer` | `1` | `0`, `1` | Measures the game's frame time with `QueryPerformanceCounter` instead of whole milliseconds. It's only used with `SimRate=0`; with `SimRate`, the precise timer is always used. |
| `FrameFix` | `1` | `0`, `1` | Some effects the game applies once per step rather than per second (steering and lean speed, spin damping, bodies coming to rest, camera zoom) are scaled to the real step length. With `SimRate=30` and `RefRate=30`, race steps are unchanged; it matters in menus and with `SimRate=0`. |
| `RefRate` | `30` | 10-240 | The step rate `FrameFix` treats those effects as tuned for. |
| `AiRate` | `30` | `0`, steps/s | Computer riders re-plan their racing line at most this many times a second (`0` = every step). It only has an effect when lower than `SimRate`, or with `SimRate=0`. |
| `AltTabFix` | `1` | `0`, `1` | Needs `InputTiming=1`. Keys and buttons held when the game goes to the background (such as Alt from Alt+Tab, look-behind by default) are released, and when it comes back any key let go meanwhile counts as let go, and a keyboard or joystick Windows took away is acquired again. The mouse pointer is also shown over the game window while it is in the background (the game's own pointer is blank). `0` = the original: such keys stay held until pressed again, the keyboard can stop working after alt-tab, and the pointer is invisible over the game window. |
| `AiPickFix` | `1` | `0`, `1` | Computer riders' random bike and rider picks can choose the last entry in the list. `0` = the original off-by-one: the last bike and rider are never picked. |
| `AiSteerRate` | `0` | `0`, degrees/s | The fastest a computer rider's front wheel can turn (`0` = no limit, as in the original). Other values change how the AI rides. |
| `RestartFix` | `1` | `0`, `1` | Restart Race puts every rider back on their start position facing the course, and FINAL LAP shows again. `0` = the original bugs: riders who had crashed or left the track restart in the wrong place or facing the wrong way. |

#### Sound and performance

| Setting | Default | Values | Description |
|---|---|---|---|
| `AsyncSound` | `2` | `0`, `1`, `2` | Where the 3D sound position update runs. The game makes it every physics step, and it's slow on Windows 10/11 because it holds the sound system's lock. `2` = a worker thread, started while the frame waits for VSync. `1` = a worker thread, started at once. `0` = the game thread, as in the original (can cause stutter). |
| `BackgroundMute` | `1` | `0`, `1` | With a replacement DirectSound such as DSOAL, the game is silenced while it is in the background (Windows' own DirectSound does this itself). Otherwise the game, which stops while in the background, leaves its sounds repeating their last fraction of a second. With any DirectSound, `1` also pauses the [music](#music) while the game is in the background. `0` = leave the sound alone. |
| `SoundMaxDistanceMute` | `1` | `0`, `1`, `2`, `3` | `1`: with a replacement DirectSound such as DSOAL, which ignores the rule, MCM2MaX mutes 3D sounds beyond their maximum distance itself, as Windows' DirectSound does. With Windows' own DirectSound it leaves them to Windows. `3` mutes them with any DirectSound (with Windows' own, the bike engines then go silent after a pause). `0` never. `2` changes nothing and only logs those sounds. |
| `FastCrt` | `1` | `0`, `1` | The game's millisecond clock uses `QueryPerformanceCounter` instead of changing the Windows timer period on every call, and its case-insensitive name comparisons use a lookup table. Both were measurable slowdowns. |
| `Simd` | `1` | `0`, `1` | MCM2MaX's own vertex, smoothing and pixel conversion work uses SSE2 when the CPU has it, after a start-up self test that checks it gives the same results as the plain code. `0` = plain code only. The game's own code (physics included) is never changed by this. |
| `SimdLog` | `0` | `0`, `1` | Log how fast the SSE2 and plain versions run on this CPU at start-up, and how much work went through them every 5 seconds. |
| `PauseOnFocusLoss` | `1` | `0`, `1` | Switching away from the game mid-race (Alt+Tab, clicking another window) opens the pause menu, as Escape does. The game's clock also stops while it is in the background, so coming back no longer stalls and then races ahead. `0` = the clock still stops, but the race isn't paused. |
| `DpiAware` | `1` | `0`, `1` | Tells Windows the game handles display scaling itself, so with scaling above 100% it runs at your screen's real resolution instead of being stretched (blurry, with a drifting menu mouse). `0` = Windows scales the game as before. |
| `RacePower` | `1` | `0`, `1`, `2` | Keeps the CPU at full speed while racing. The game needs so little CPU that on the Balanced power plan Windows lowers the clock while it waits for VSync, and some frames then run late (regular stutter, mostly on older CPUs). `1` = switch Windows to the High performance power plan when a race starts and back to your plan afterwards (if the game crashes, it is put back at the next start). `2` = High performance the whole time the game runs. `0` = off. Nothing is changed on battery or when the plan is already High performance. |
| `RacePowerAsync` | `1` | `0`, `1` | `1` = the power plan switches run on their own thread, so the game doesn't freeze while Windows changes plan (switching back to Balanced can take about 2 seconds, which froze the countdown and the menus after a race). `0` = on the game thread, as before. |
| `FastTerrain` | `1` | `0`, `1` | The game's per-frame terrain code (how detailed each block is, which of its points are used, building its triangles when that changes, and the walk that draws it) runs as MCM2MaX's own rewrite: exactly the same results, bit for bit, but it reads ahead in memory and avoids the old maths' slow spots. At long view distances this was the biggest part of the game's own work per frame. Ctrl+F8 switches it on and off while racing (shown on screen). `0` = the game's own code. |
| `TerrainMeshCache` | `32768` | `256`-`1048576` | Memory in KB the game may keep for terrain triangles it has built. The game kept only 1 MB, so at long view distances it threw away and rebuilt hundreds of terrain blocks every frame. `1024` = the game's own limit. |
| `GpuTerrain` | `1` | `0`, `1` | Direct3D 11 only: the whole track's terrain is built once at full detail and kept on the graphics card, in place of the game's per-frame terrain. It looks the same as the game's terrain, with more detail in the distance, and is much faster at long view distances. The ground you ride on is unchanged (the game's collision already uses the full-detail heights). Direct3D 9 keeps the game's terrain. Ctrl+F7 switches between it and the game's terrain while racing. |
| `FastFtol` | `1` | `0`, `1` | The game's float-to-integer conversion, used all over its code, is one SSE3 instruction instead of two FPU mode switches around a store. Exactly the same results. CPUs without SSE3 keep the old code. |
| `FastGameCode` | `1` | `0`, `1` | A few of the game's own loops that its 1999 compiler built badly are rewritten in place with the same maths, so results are exactly the same, only faster. So far: the terrain texture coordinate loop, which ran every frame and stalled the CPU, and the per-frame tree cache scan, which now reads ahead instead of waiting on memory. |
| `FastSort` | `1` | `0`, `1` | The game's sorting uses a merge sort instead of the old C library qsort, which slows to a crawl when many items are equal. On tree-heavy maps that froze the game for 35-125 ms when turning the camera. Being stable, it also stops distant riders' engine sounds retriggering as quiet looping snippets. |
| `TreeCacheMB` | `32` | `0`, 1-512 | The game's cache of built trees is only emptied when more than this many MB of it are unused (the original 256 KB emptied it constantly while turning, and the trees were then rebuilt). `0` = the original. |
| `StateCache` | `1` | `0`, `1` | Direct3D 11 only: renderer state that's already set isn't sent again for every draw. `0` is slower and is for troubleshooting rendering problems. |
| `DrawBatch` | `1` | `0`, `1` | 3D draws in a row with nothing changed between them (trees, terrain, scenery) are sent as one draw. `0` sends each draw on its own, for troubleshooting. |
| `StillScenery` | `1` | `0`, `1` | Terrain and close-up tree draws skip the high frame rate smoothing's checks: the game never moves them, but the checks compared every point of them every frame, which added up at long view distances. |
| `FastObjectWalk` | `1` | `0`, `1` | The game's two walks over the objects in view (trees, then fences, signs and buildings) share one list made by the first walk, so the second no longer visits every tree just to skip it. Same objects in the same order. Helps most with a long `ViewDistance`. |
| `FarTrees` | `1` | `0`, `1` | Distant trees are drawn by the graphics card in one go, from numbers written down once per tree, instead of the game building every billboard again each frame. They look the same; the game still draws the trees near the camera. Needs `FastObjectWalk=1` and Direct3D 11. `FarTreeDistance` (default `0` = automatic, one and a half times the distance of the 3D trees) sets from how far. |
| `FarTreeCells` | `1` | `0`, `1` | With `FarTrees`, parts of the map holding nothing but far trees are left out of the game's per-frame walk over the objects in view (the graphics card draws their trees anyway). |

### 3. Logging

`Log=1` writes one short log per run; please include the newest one with any
problem report. Everything else here is for debugging: it slows the game down
and makes the log much bigger. All of it is off by default, so turn one on
only when asked to. Logs go to `%LOCALAPPDATA%\Motocross Madness 2\logs`.

| Setting | Default | Description |
|---|---|---|
| `Log` | `1` | Writes `logs\MCM2MaX-<date>-<time>.log` each run (the newest 50 are kept): Windows version, these settings, the renderer chosen and any problems. If the game crashes it also records where and why, and writes a crash dump (`.dmp`) next to the log. `0` = no log. |
| `D3D9Log` | `0` | Direct3D 9 renderer details: card capabilities, the first frames, fallbacks. |
| `AiLog` | `0` | Computer rider speeds every 5 seconds, plus every jump (air time, speed at takeoff and landing) and respawn, for computer riders and the player. |
| `DDStats` | `0` | Renderer activity (draws, uploads, surfaces) every 2 seconds. |
| `Profile` | `0` | Performance profile: frame times, timed sections, GPU time and a sampling profile of the game thread, in `logs\perf-*.csv` and `logs\perf-samples-*.txt` (summarise with `tools/perf_report.py`). It costs some frame rate. |
| `ProfileSampleUs` | `1000` | With `Profile=1`, how often the game thread is sampled, in microseconds. `0` = timings only. |
| `ActivateLog` | `0` | Window activation (the game pauses while in the background). |
| `RegLog` | `0` | Every registry call the game makes. |
| `TextLog` | `0` | GPU text drawing and what erases it. |
| `BatchLog` | `0` | Every 5 seconds: how many draws `DrawBatch` merged and what ended each batch. |
| `DrawLog` | `0` | During races, which draw calls could be merged into fewer (per game call site, plus a few whole frames). Costs about 1.5 ms a frame. |
| `GdiLog` | `0` | The GDI calls (text, boxes, lines) the game draws onto the screen with. |
| `TextureLog` | `0` | The format of each texture the game creates. |
| `InputLog` | `0` | With `InputTiming`, the keyboard and joystick input each race step gets: events given out early or held for a later step, held taps, joystick samples, and a summary every 10 s. |
| `XInputLog` | `0` | With `XInput`, the game's calls to the Xbox controllers (properties, force feedback effects), button presses, and every 10 s a summary per controller (axes, events, rumble). |
| `SortLog` | `0` | The game's sorts per calling code every 10 s (count, largest, time). Sorts over 2 ms are always logged. |
| `WalkLog` | `0` | With `FastObjectWalk`, every 5 seconds how many object lists were made and reused, and now and then a check of a reused list against a fresh walk (costs frame rate). |
| `FarTreeLog` | `0` | With `FarTrees`, every 5 seconds how many trees the graphics card drew and how many were left to the game, plus checks of the game's billboards against the far-tree numbers. `2` also tints GPU-drawn trees red. |
| `TerrainCheck` | `0` | With `FastTerrain`, every 2 seconds one frame also runs the game's own terrain code and logs any difference from the rewrite (there should be none), with a summary every 5 seconds. Costs a few milliseconds every 2 seconds. |
| `GpuTerrainDetail` | `1` | With `GpuTerrain`, the game's fine detail texture is laid over the terrain, as the game does close up. `0` = without it. |
| `GpuTerrainDetailReach` | `0` | How far from the camera the detail texture reaches (it fades out over the last quarter). `0` = all the way, as the game shows it. |
| `GpuTerrainFar` | `1` | With `GpuTerrain`, terrain beyond the distance where the game switches its blocks to "far" is drawn as the game draws it there: without the detail texture, in the game's far colours. `0` = the near look all the way. |
| `GpuTerrainLog` | `1` | With `GpuTerrain`, how the terrain was built and every 5 seconds what was drawn. `2` also tints the terrain by detail level. |
| `FreeCam` | `1` | Ctrl+F6 during a race freezes it and gives a free camera for screenshots: mouse or arrow keys look, W/A/S/D move, Space/E up, C/Q down, Shift faster, Z slower. Ctrl+F6 again and the race carries on. |
| `FreeCamSpeed` | `60` | Free camera speed in feet per second. |
| `FreeCamMouse` | `15` | Free camera mouse sensitivity. |
| `TerrainLog` | `0` | Terrain statistics every 5 seconds, with debug keys: Ctrl+F9 switches `TerrainHoleFix` on and off, Ctrl+F10 steps `TerrainDetail` (shown on screen), Ctrl+F11 writes the terrain to the log and probes the point under the cursor (Direct3D 11). `2` also looks for holes in the terrain every 2 seconds. |
| `SoundLog` | `0` | 3D sound buffers, and when `SoundMaxDistanceMute` mutes or unmutes them. |
| `SoundPauseLog` | `0` | When a race is paused and unpaused: every DirectSound call for a few seconds, failed calls, and the state of every sound buffer and of the window DirectSound follows. |
| `RaceTitleLog` | `0` | The countdown race title: name and lap count found, timing and placement. |
| `RestartLog` | `0` | The start grid when a race loads and on Restart Race. |
| `ArrowLog` | `0` | The direction arrow's placement for a few seconds after each race start. |
| `FovLog` | `0` | Every change to a camera's field of view: who set it and what MCM2MaX widened it to. |
| `AlphaLog` | `0` | Each kind of see-through draw, and whether `AlphaCut` or `AlphaToCoverage` applied to it, plus alpha-to-coverage counts every 10 seconds. |
| `ReflectLog` | `0` | Which bike and rider textures and model parts reflect, the sky tiles kept for `Reflections`, and every 5 seconds what the reflections cost. |
| `ReflectDebug` | `0` | `1` = the reflection alone at full strength on everything that reflects (no Fresnel), to see what is reflected. |
| `MotionBlurLog` | `0` | With `MotionBlur`, every 5 seconds: speed, blur amount and focus point, where in the frame the blur went, plus a few whole-frame draw lists. |
| `MenuLog` | `0` | Log every event on the Options pages MCM2MaX adds settings to. |
| `ShadowLog` | `0` | `RiderShadows` counters every 5 seconds: shadows drawn, vertices, time taken, terrain draws shadowed. `2` also logs every shadow's placement for 90 frames, 10 s into each race. |
| `SmoothLog` | `0` | `SimSmooth` tracing: `1` = matrices and draws for a few frames, `2` = camera only (120 frames), `3` = rider shadows, `4` = camera and projection after each race start. |

### 4. Music (`[Music]` section)

See [Music](#music). Every music key is empty in the release, so no music
plays until you set one. The music keys are read again at each race and each
return to the menus; the other settings here when the game starts.

| Setting | Default | Values | Description |
|---|---|---|---|
| `Menu` | empty | files, folders | Music in the menus. A value is a `.wav`, `.flac` or `.mp3` file or a folder (every such file in it), relative to the game folder (or `%LOCALAPPDATA%\Motocross Madness 2`) or a full path; separate several with `;`. A list plays in turn and repeats; a single file loops. |
| `Race` | empty | files, folders | Music in any race without its own event or track music. Empty = silent races. |
| `Baja`, `Enduro`, `Nationals`, `Quarry`, `Supercross`, `Tag` | empty | files, folders | Music for every track of that kind of event, instead of `Race`. Add-on tracks count as the event whose `Teraform` folder they are in. |
| *track name* | not set | files, folders | Music for one track, instead of its event's. The key is the track's file name in `Teraform` without `.env`: `BAJA01`-`BAJA05`, `AIRFIELD`, `FARM`, `OPENPIT`, `SKILODGE`, `TRAILER`, `CHANOS`, `HILLSIDE`, `IFFENDIC`, `NAT01`-`NAT12`, `VV`, `QUARRY01`-`QUARRY05`, `SX01`-`SX17`, `TAG01`-`TAG03`, or an add-on track's name. |
| `MusicVolume` | `80` | 0-100 | Music volume in the menus. |
| `RaceMusicVolume` | empty | 0-100 | Music volume in races. Empty = `MusicVolume`. |
| `Shuffle` | `1` | `0`, `1` | A list plays in random order, a new order each time round. `0` = in order (a folder's files by name). |
| `PauseInRace` | `1` | `0`, `1` | Race music pauses while the race is paused. `0` = it keeps playing. (Music always pauses while the game is in the background when `BackgroundMute=1`.) |
| `FadeTime` | `1000` | 0-10000 | Milliseconds to fade the music out when it changes, such as from the results back to the menus. Loading a track and finishing a race always fade in 0.3 s. |
| `Log` | `0` | `0`, `1` | Logs every music file, loop and state change, and a playback summary every 30 s. Starts, stops and problems are always logged while music is set. |

The shipped `MCM2MaX.ini` is generated from the block in
[code/README.md](code/README.md) by `tools/package.sh`, and the parser is
[code/src/config.cpp](code/src/config.cpp) ([code/src/music.cpp](code/src/music.cpp) for `[Music]`).

## Texture dumping and replacement

1. Set `TextureDump=1`, play, and every texture the game loads is saved to
   `%LOCALAPPDATA%\Motocross Madness 2\textures_dump\<name>.png`, at the
   largest size the game loaded it.
   - Named textures (from the `.RES` archives or TGA files) use the game's
     name, lower case, without extension, with folder separators turned into
     `_`. Terrain textures are named `<file>_<offset>`.
   - Textures without a name are saved as `<hash>.png` (16 hex digits). They
     are replaced only when the game loads exactly those pixels.
2. Edit or upscale a file and keep its name. Put it in `textures\` in the
   game folder, or in `%LOCALAPPDATA%\Motocross Madness 2\textures\`.
3. With `TextureReplace=1` (the default) the file replaces that texture at
   every size. PNG, TGA, JPEG and BMP of any size work, at full 32-bit colour,
   and mipmaps are generated on the GPU. Keep alpha where the original has
   it.

Videos, menu screens, the cursor and fonts aren't textures, so they can't be
replaced. Hashes change with `TrueColorTextures`, so dump again if you
switch it. `tools/sky_tiles.py` stitches a track's sky tiles into one
picture for editing and splits it back.

## Problems and logs

Each run writes `%LOCALAPPDATA%\Motocross Madness 2\logs\MCM2MaX-<date>-<time>.log`.
The log starts with the Windows version, the settings in effect, the renderer
chosen and any fallbacks taken. Lines with "not applied" or "skipped" mean a
patch didn't match. Please attach the newest log to any report.

Common things to check:
- **Nothing happens / the game exits at once**: check that the log says the
  exe is supported. A different `Mcm2.exe` (for example the SafeDisc CD exe)
  is left alone.
- **No sound reverb**: that's expected without DSOAL. See
  [Sound, EAX and DSOAL](#sound-eax-and-dsoal).
- **DSOAL not taking effect**: make sure you used the 32-bit DLLs.
- **Tearing**: set `VSync=1`, or try `Fullscreen=Exclusive`.

## Building from source

The build is 32-bit only. CI (`.github/workflows/build.yml`) builds with both
MSVC and MinGW, checks the MinGW DLL for Windows XP imports, and uploads the
release zip as `MCM2MaX-release`.

MinGW cross-compile on Linux (the release build, with the video decoder and
XP support):

    sudo apt-get install g++-mingw-w64-i686 cmake zip
    tools/build-ffmpeg.sh ffmpeg-work
    cmake -S code -B build -DCMAKE_TOOLCHAIN_FILE=code/toolchain-mingw32.cmake \
          -DCMAKE_BUILD_TYPE=Release -DMCM2MaX_FFMPEG=$PWD/ffmpeg-work/install
    cmake --build build
    tools/package.sh build/MCM2MaX.dll MCM2MaX.zip

Visual Studio (no videos, no XP):

    cmake -S code -B build -A Win32
    cmake --build build --config Release

`-DMCM2MaX_TESTS=ON` builds `dd_smoke.exe`, which renders a test scene
through both renderers without the game, and runs under Wine. More detail is
in [code/README.md](code/README.md).

## Repository layout

| Path | Contents |
|---|---|
| `code/src/` | MCM2MaX: game patches (`fixes_*.cpp`), frame pacing (`framerate.cpp`, `smooth*.cpp`), music (`music.cpp`), profiler (`prof.cpp`), XP shims (`xp_compat.cpp`) |
| `code/src/dd/` | DirectDraw 7 / Direct3D 7 on Direct3D 11 (`backend.cpp`, `device.cpp`) and Direct3D 9 (`backend9.cpp`, `device9.cpp`), GPU text, HUD text, MSAA, texture replacement |
| `code/src/video/` | AVI reader, MS ADPCM decoder, waveOut audio, Indeo 5 through FFmpeg |
| `code/src/music*.cpp` | menu and race music (`[Music]`): waveOut player, WAV/FLAC/MP3 decoding |
| `code/third_party/dr_libs/` | dr_wav, dr_flac, dr_mp3 (public domain / MIT-0) |
| `code/tests/` | renderer smoke test |
| `tools/` | `package.sh` (release zip), `build-ffmpeg.sh`, `mcm2patch.py` (exe patcher), `hlp2html.py` (WinHelp to HTML), `check-xp-imports.py`, `perf_report.py`, `sky_tiles.py` |
| `package/` | player README, `MCM2HELP.html`, HUD fonts, licences |
| `docs/` | [why the game quit on Windows 11](docs/step1-launch-crash.md), [runtime writes](docs/runtime-writes.md), [the D3D11 renderer](docs/step3-renderer.md), [Windows XP](docs/windows-xp.md), [clean-machine release test](docs/clean-machine-test.md) |

## Licences

- The video decoder is [FFmpeg](https://ffmpeg.org) n7.1, built with only its
  Indeo 5 decoder and linked statically, under the GNU LGPL 2.1 or later
  (`licenses/FFmpeg-LGPL-2.1.txt`). `tools/build-ffmpeg.sh` rebuilds it, so
  the DLL can be relinked against a modified FFmpeg.
- WAV, FLAC and MP3 decoding for the music uses
  [dr_libs](https://github.com/mackron/dr_libs) (dr_wav 0.14.6, dr_flac
  0.13.4, dr_mp3 0.7.4) by David Reid, public domain or MIT No Attribution
  (`code/third_party/dr_libs`).
- The HUD fonts are Contrail One (c) 2011 Sorkin Type Co and DSEG7 Classic
  (c) 2017 keshikan, both under the SIL Open Font License 1.1
  (`licenses/ContrailOne-OFL.txt`, `licenses/DSEG-OFL.txt`).
- Motocross Madness 2 is (c) Microsoft / Rainbow Studios. This repository
  and the release contain no game files.
