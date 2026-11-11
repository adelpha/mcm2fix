# mcm2fix: Motocross Madness 2 for Windows 10 and 11

mcm2fix makes **Motocross Madness 2** (Microsoft / Rainbow Studios, 2000) run
on modern Windows by double-clicking `Mcm2.exe`. You don't need compatibility
settings, administrator rights, DirectX wrappers or old codecs, and you don't
change the game's display resolution.

It is a single DLL that the game loads in place of `d3drm.dll`, a library
Windows no longer ships. When the DLL loads, it patches the game's code in
memory. It brings its own DirectDraw 7 / Direct3D 7 implementation that draws
with **Direct3D 11**, and it falls back to **Direct3D 9** where Direct3D 11 is
missing. Your `Mcm2.exe` and the other game files are never modified.

The same DLL also runs on **Windows XP SP3**, where it uses Direct3D 9.

---

## Contents

- [What it fixes](#what-it-fixes)
- [Features](#features)
- [Requirements](#requirements)
- [Installing](#installing)
- [Where your settings, saves and logs go](#where-your-settings-saves-and-logs-go)
- [Uninstalling](#uninstalling)
- [Renderers and fullscreen](#renderers-and-fullscreen)
- [Windows XP](#windows-xp)
- [Sound, EAX and DSOAL](#sound-eax-and-dsoal)
- [mcm2fix.ini reference](#mcm2fixini-reference)
- [Texture dumping and replacement](#texture-dumping-and-replacement)
- [Problems and logs](#problems-and-logs)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [Licences](#licences)

---

## What it fixes

These are the problems that stop the original game, or make it misbehave, on
Windows 10 and 11.

| Problem in the original game | What mcm2fix does |
|---|---|
| The game imports `d3drm.dll`, which Windows 10/11 don't ship, so it won't start. | mcm2fix *is* `d3drm.dll`. It provides the one function the game used (`D3DRMVectorRotate`). |
| The game quits silently at startup. Right after switching to 640x480, `Flip` returns `DDERR_SURFACELOST` and the game treats it as fatal. | Its renderer never changes the display mode. The flip loop also restores and retries on a lost surface. |
| An administrator check writes to HKLM and creates a temp file in the game folder, and the game exits if either fails. | Removed. The game runs as a standard user. |
| A race between threads on multi-core CPUs hangs the game on the logo or fails startup. | Fixed in the renderer and startup path. `OneCore=1` keeps the old single-core workaround available. |
| Settings are written to `HKLM`, and replays, profiles, saves and logs go into the game folder. Both fail without admin rights, or land in the VirtualStore. | Settings go to `HKCU`. File writes go to a per-user folder that overlays the game folder (see [below](#where-your-settings-saves-and-logs-go)). |
| A video card picker and a "hardware has changed" dialog appear. | No picker. The best Direct3D adapter is chosen automatically and the dialog is skipped. |
| Saved hardware-test results (`DriverInfo`) could leave the window not redrawing. | The cache is dropped whenever the list of display modes changes. |
| Videos (intro, credits, menu previews) need the Indeo 5 codec and DirectShow, which modern Windows don't have. | mcm2fix has its own AVI player: Indeo 5 through FFmpeg, MS ADPCM sound, played with waveOut. No codec install is needed. |
| In-game Help opens `MCM2HELP.HLP`, and Windows 10/11 can't open WinHelp files. | The Help buttons open `MCM2HELP.html`, a full conversion with pages, pictures, stunt videos, index and search. |
| The game asks for video memory as a signed number, so a card with 2 GB or more makes widescreen modes unusable. | The renderer reports 256 MB. |
| Only 4:3 resolutions are offered. | Widescreen modes up to your desktop size are offered, and the field of view widens to match. |
| Game speed, AI and physics break at high frame rates. | Races run at a fixed 30 simulation steps per second with smooth blended drawing (see [Frame pacing](#frame-pacing-and-smoothness)). |
| **Restart Race** from the pause menu puts riders on the wrong grid spots, facing the wrong way, and FINAL LAP is not shown again. | Fixed (`RestartFix`). |
| The race direction arrow leans toward the screen centre instead of pointing at the next gate. | Fixed, with a slight top-down tilt and a larger size (`Arrow*`). |
| Trees and flags show a stair-step fringe that hides things behind them. | Fixed by alpha-test threshold (`AlphaCut`), and transparent texels no longer bleed grey into leaf edges. |
| Texture formats are always 16-bit, because of a bug in the game's 32-bit format description. | Fixed (`TrueColorTextures`), plus full 32-bit texture replacements. |
| With DSOAL, distant riders leave quiet looping snippets of old sounds. | mcm2fix mutes 3D sounds past their max distance itself (`SoundMaxDistanceMute`). |

## Features

### Display
- **Borderless window at your desktop resolution** on the primary monitor.
  The game's display mode is virtual: the picture is rendered at the
  resolution you choose in the game's options and scaled to the screen with
  its aspect ratio kept. The desktop never changes resolution.
- **Optional exclusive fullscreen** (`Fullscreen=Exclusive`), at the desktop
  mode or any mode you name (`FullscreenMode=1024x768@85`).
- **Widescreen resolutions**: 1280x720, 1280x800, 1366x768, 1440x900,
  1600x900, 1680x1050, 1920x1080, 1920x1200, 2560x1080, 2560x1440,
  2560x1600, 3440x1440, 3840x1600 and 3840x2160 are offered alongside the
  original 4:3 modes. Only sizes that fit your desktop are listed, and your
  exact desktop size is always one of them.
- **Widescreen field of view** (`WideFov`): wider screens show more to the
  sides instead of cropping the top and bottom. The speedometer stays round.
  `Fov` sets the base field of view.
- **VSync and frame cap** (`VSync`, `FrameCap`).

### Graphics
- **Multisample antialiasing** of the 3D scene, up to 8x (`Antialiasing`,
  default 4x).
- **Anisotropic filtering**, up to 16x (`Anisotropy`, default 16x).
- **View distance, fog distance and model detail** multipliers
  (`ViewDistance`, `FogDistance`, `ModelLod`), plus texture sharpness
  (`MipBias`).
- **True-colour textures** where the game's own art has more than 16 bits
  (`TrueColorTextures`).
- **Texture dumping and replacement** by name with PNG, JPEG or BMP files of
  any size (`TextureDump`, `TextureReplace`). See
  [below](#texture-dumping-and-replacement).
- **Sharp text**. The game draws its menu and HUD text with GDI on 640x480
  surfaces. mcm2fix draws that text on the GPU instead, antialiased, at your
  screen's resolution (`GpuText`).
- **New race HUD**. The position, lap and time panels are drawn larger
  (`HudScale`) in new fonts: Contrail One for words and DSEG7 Classic for
  digits (`HudFont`, `HudDigitFont`). The fonts are loaded by the game only
  and nothing is installed in Windows. Rider name labels are larger too
  (`LabelScale`), and the speedometer needle is an antialiased thicker line
  (`LineWidth`).

### Frame pacing and smoothness
- **Fixed-rate simulation** (`SimRate=30`). The game's physics and AI were
  tuned for low frame rates and go wrong at 144 Hz. Races now always step at
  30 per second, whatever the frame rate.
- **Smooth drawing between steps** (`SimSmooth`). The camera, bikes, riders,
  shadows and name labels are drawn blended between the last two simulation
  steps, so motion is smooth at any refresh rate (one step of delay).
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
- Direct3D 11 state caching (`StateCache`), batched GPU text and cheaper
  texture uploads.
- A built-in profiler for finding hitches (`Profile`).

### Other
- **In-game Help** opens `MCM2HELP.html` in your web browser.
- **Renderer label**: "Direct3D 11" or "Direct3D 9" in the bottom right
  corner of the menus (`RendererLabel`).
- **Version check**: mcm2fix only patches the supported `Mcm2.exe`. With any
  other exe it does nothing and says so in the log.

## Requirements

- **The game files** installed in a folder, with this `Mcm2.exe`:
  - size **1,577,005 bytes**
  - SHA-256 `345e4f644fe2237108472acbcde3123b186d2475dd5a23480321c78084746f9e`

  This is the widely available no-CD exe. The original CD `Mcm2.exe` uses
  SafeDisc copy protection, which Windows 10 and 11 cannot run at all.
- **Windows 10 or 11** with a Direct3D 11 graphics card, or Windows 7/8.
  Windows XP SP3 also works; see [Windows XP](#windows-xp).
- Nothing else. You don't need the DirectX 7 runtime, `d3drm.dll` from an
  old Windows, the Indeo codec or compatibility settings. If you set
  compatibility modes on `Mcm2.exe` earlier, it's best to remove them.

## Installing

1. Get the release zip (`mcm2fix-release` from the
   [GitHub Actions builds](../../actions), or a release package).
2. Copy everything in it into the game folder, next to `Mcm2.exe`:

   | File | What it is |
   |---|---|
   | `d3drm.dll` | mcm2fix itself |
   | `mcm2fix.ini` | settings, each one explained, all at their defaults |
   | `MCM2HELP.html` | the converted in-game help |
   | `FONTS\` | the HUD fonts (Contrail One, DSEG7 Classic) |
   | `README.txt` | short player notes |
   | `licenses\` | FFmpeg and font licences |

3. Double-click `Mcm2.exe`.

The game loads a library called `d3drm.dll`, and Windows looks in the game
folder first, so the fix is picked up with the exe left untouched.

`mcm2fix.ini` is optional. A missing file or a missing setting uses the
default. The game folder can be read-only (for example under
`Program Files`), because nothing is written there.

**On first run** mcm2fix copies any existing settings from the old
`HKLM\Software\Microsoft\Microsoft Games\Motocross Madness 2` key (an
earlier install) into its own per-user key, and sets the install path from
the folder `Mcm2.exe` is in.

*Alternative for developers:* `python tools/mcm2patch.py Mcm2.exe
Mcm2_fixed.exe` makes a copy of the exe that imports `mcm2fix.dll` by that
name instead of `d3drm.dll`. Then put `mcm2fix.dll` next to it.

## Where your settings, saves and logs go

| What | Where |
|---|---|
| Game settings (options, controls, resolution, last profile) | `HKEY_CURRENT_USER\Software\mcm2fix\Motocross Madness 2` |
| Profiles, replays, ghosts, user tracks, circuits, the game's own .ini and log files | `%LOCALAPPDATA%\Motocross Madness 2\` |
| mcm2fix logs (one per run, newest 50 kept) | `%LOCALAPPDATA%\Motocross Madness 2\logs\mcm2fix-YYYYMMDD-HHMMSS.log` |
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

Delete `d3drm.dll`, `mcm2fix.ini`, `MCM2HELP.html`, `FONTS\`, `README.txt`
and `licenses\` from the game folder. To also remove your settings and saves,
delete `HKCU\Software\mcm2fix` and `%LOCALAPPDATA%\Motocross Madness 2`.

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
| `Exclusive` | The game owns the screen, at the desktop mode or `FullscreenMode=<w>x<h>[@<Hz>]`. The game's picture is still scaled into it. Alt+Tab leaves fullscreen, and fullscreen comes back when you return to the game. This can help with VSync on Windows XP. |

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
  mcm2fix waits for the vertical blank itself (`VSync=1`).
  `Fullscreen=Exclusive` avoids the issue entirely.
- Texture dump/replacement uses WIC, which XP has only with .NET 3.0+ or the
  WIC redistributable. Without it those features log a failure and do
  nothing.

Details: [docs/windows-xp.md](docs/windows-xp.md).

## Sound, EAX and DSOAL

The game uses DirectSound 3D. On Windows 10/11, DirectSound is emulated in
software and reports no hardware 3D voices, so the game's **EAX** reverb
never turns on.

[DSOAL](https://github.com/kcat/dsoal) (DirectSound on OpenAL Soft) brings
back hardware-style 3D sound and EAX. Put its **32-bit (Win32)** DLLs in the
game folder. The 64-bit ones are silently ignored by the 32-bit game, which
then falls back to Windows' own DirectSound. With DSOAL the game finds EAX at
startup and applies its per-track reverb. mcm2fix works with or without
DSOAL.

DSOAL ignores the `DSBCAPS_MUTE3DATMAXDISTANCE` flag that the game sets on
all its 3D sounds. Riders who were out of earshot then kept playing quiet
looping fragments of old sounds. `SoundMaxDistanceMute=1` (the default) has
mcm2fix mute 3D sounds beyond their max distance itself, as Windows'
DirectSound does. The game's own sounds fade to -20 dB at max distance and
then cut off, so a slight snap-off at the edge is original behaviour.

## mcm2fix.ini reference

`mcm2fix.ini` sits next to `Mcm2.exe`, in a section called `[mcm2fix]`. All
settings are optional and fall back to the defaults below. On/off settings
take `1` or `0`. A `; comment` after a value is ignored.

### Core fixes

| Setting | Default | Description |
|---|---|---|
| `Log` | `1` | Write a log per run to `%LOCALAPPDATA%\Motocross Madness 2\logs\` (newest 50 kept). |
| `Startup` | `1` | Pick the video card automatically and skip the "detected a change" box. |
| `Registry` | `1` | Keep the game's settings in `HKCU\Software\mcm2fix\Motocross Madness 2` instead of HKLM. |
| `Files` | `1` | Send the game's file writes to `%LOCALAPPDATA%\Motocross Madness 2` (overlay on the game folder). |
| `OneCore` | `0` | Pin the game to one CPU core. This was the old workaround for the startup race and isn't needed now. |

### Display and renderer

| Setting | Default | Description |
|---|---|---|
| `Renderer` | `D3D11` | `D3D11` or `D3D9`. D3D9 is used automatically when Direct3D 11 is missing. |
| `Fullscreen` | `Borderless` | `Borderless` (a window covering the screen) or `Exclusive` (owns the screen; Alt+Tab leaves it). |
| `FullscreenMode` | `Desktop` | Exclusive only: `Desktop` (keep the current mode) or `<width>x<height>[@<Hz>]`, e.g. `1024x768@85`. |
| `VSync` | `1` | Wait for the monitor's refresh. `0` = no wait (tears if the system allows it). |
| `FrameCap` | `0` | Most frames per second. `0` = no cap beyond VSync. |
| `WideFov` | `1` | Widescreen modes show more to the sides. `0` = crop top and bottom like the original. |
| `Fov` | `0` | Field of view in degrees as a 4:3 screen shows it, 20-150. `0` = game default (77). The zoom keys still step from it. |
| `RendererLabel` | `1` | Show the active renderer (Direct3D 9 / 11) in the bottom right corner of the menus. |

### Graphics quality

| Setting | Default | Description |
|---|---|---|
| `Antialiasing` | `4` | Multisample antialiasing of the 3D scene: `0`/`1` = off, `2`, `4` or `8`. |
| `Anisotropy` | `16` | Anisotropic texture filtering: `0`/`1` = off, `2`-`16`. |
| `MipBias` | `0` | Texture sharpness at a distance, -4 to 4. Negative = sharper (e.g. `-0.5`), positive = softer. |
| `ViewDistance` | `1` | Multiplies how far the game draws, 0.25-10. Far clip, object culling and fog move together. |
| `FogDistance` | `1` | Multiplies the fog distances on top of `ViewDistance`, 0.1-20 (`2` = fog starts twice as far away). |
| `ModelLod` | `1` | Detailed models stay this many times further away before switching to simpler ones, 0.25-10. |
| `TreeDetailDistance` | `1` | `0` = every tree uses its sparse close-up version at any distance (looks bad; for fun). |
| `AlphaCut` | `128` | Trees and flags: drop pixels less opaque than this (0-255), which removes the fringe where trees hide flags behind them. `0` = original look. |
| `TrueColorTextures` | `1` | Load textures in full colour (24-bit, 32-bit with alpha) instead of the game's 16-bit, where the source has more than 16 bits. |
| `TextureReplace` | `1` | Draw pictures from `textures\` in place of the game's textures. |
| `TextureDump` | `0` | Save every texture the game loads to `%LOCALAPPDATA%\Motocross Madness 2\textures_dump`. |

### Text and HUD

| Setting | Default | Description |
|---|---|---|
| `GpuText` | `1` | Draw the game's menu and HUD text (GDI) on the GPU, antialiased, at screen resolution. |
| `HudText` | `1` | With `GpuText`: the race HUD panels' text is drawn sharp by mcm2fix. |
| `HudScale` | `2` | Race HUD text panels (position, lap, times) drawn this many times larger. `1` = original. |
| `LabelScale` | `1.5` | Name labels over the riders drawn this many times larger. `1` = original. |
| `LineWidth` | `3` | Speedometer needle width in pixels, antialiased, 1-16. `1` = original thin line. |
| `HudFont` | `FONTS\ContrailOne-Regular.ttf` | A .ttf (relative to the game folder) for the HUD panel and name label text. Empty = the game's fonts. |
| `HudDigitFont` | `FONTS\DSEG7Classic-BoldItalic.ttf` | A .ttf for the numbers in the stats panel (times, position, lap). Words keep `HudFont`. Empty = same as `HudFont`. |
| `HudDigitSize` | `0.7` | Size of those numbers relative to the words beside them, 0.5-2. |
| `HudStuntTextSize` | `0.8` | Stunt events: size of the score panel's text, 0.5-1.5 (`1` = as big as the race panel's). |

### Frame pacing and simulation

| Setting | Default | Description |
|---|---|---|
| `SimRate` | `30` | Races run this many simulation steps per second whatever the frame rate, 10-1000. `0` = one step per frame (the original way). 30 is the tested value. |
| `SimSmooth` | `1` | Draw the camera, bikes, riders, shadows and names blended between simulation steps (smooth at any frame rate, one step of delay). `0` = show each step as it is. |
| `PreciseTimer` | `1` | The game's frame time comes from `QueryPerformanceCounter` instead of whole milliseconds. |
| `FrameFix` | `1` | Steps the game applies once per frame scale with frame time. `0` = original. |
| `RefRate` | `30` | The frame rate those per-frame steps are treated as tuned for, 10-240. |
| `AiRate` | `30` | Computer riders re-plan their line this many times a second. `0` = every frame. |
| `AiSteerRate` | `0` | Fastest a computer rider's front wheel can turn, degrees per second. `0` = no limit. |

### Gameplay fixes

| Setting | Default | Description |
|---|---|---|
| `RestartFix` | `1` | Restart Race puts every rider back on the grid facing the course, and FINAL LAP shows again. `0` = original bug. |
| `ArrowFix` | `1` | The race direction arrow points the right way. `0` = original (leans toward the screen centre). |
| `ArrowTurn` | `0.8` | How much of that correction to apply, 0-2 (`1.0` = full; lower it if the arrow points left of a gate dead ahead). |
| `ArrowTilt` | `10` | Degrees the arrow's nose is raised so it is seen a little from above, -45 to 45 (`0` = level). |
| `ArrowScale` | `2.0` | Size of the race direction arrow, 0.25-8 (`1.0` = original). |

### Sound and performance

| Setting | Default | Description |
|---|---|---|
| `AsyncSound` | `2` | The 3D sound listener update the game makes every physics step runs on a worker thread, started while the frame waits for VSync. `1` = started at once, `0` = on the game thread. |
| `SoundMaxDistanceMute` | `1` | 3D sounds further away than their max distance are muted by mcm2fix, as Windows' DirectSound does (DSOAL doesn't). |
| `FastCrt` | `1` | The game's millisecond clock reads `QueryPerformanceCounter` (no timer-period change per call), and its case-insensitive name compares use a table. `0` = the game's own code. |
| `StateCache` | `1` | Direct3D 11: state already bound isn't sent again for every draw. |

### Debugging

These settings are for diagnosing problems. Each one adds lines to the log,
or files to `logs\`.

| Setting | Default | Description |
|---|---|---|
| `D3D9Log` | `1` | Extra Direct3D 9 renderer logging (device caps, first frames, fallbacks). |
| `DDStats` | `0` | Log renderer activity every 2 seconds. |
| `Profile` | `0` | Performance profile: frame times, timed sections, GPU time and a sampling profile of the game thread, in `logs\perf-*.csv` / `perf-samples-*.txt` (summarise with `tools/perf_report.py`). |
| `ProfileSampleUs` | `1000` | How often (microseconds) `Profile` samples the game thread. `0` = no sampling. |
| `ActivateLog` | `0` | Log window activation. |
| `RegLog` | `0` | Log every registry call. |
| `TextLog` | `0` | Log GPU text drawing and what erases it. |
| `GdiLog` | `0` | Log the GDI calls (text, boxes, lines) the game draws onto the screen with. |
| `TextureLog` | `0` | Log the format of each texture the game creates. |
| `SoundLog` | `0` | Log 3D sound buffers and when they are muted / unmuted. |
| `RestartLog` | `0` | Log the start grid at race load and on Restart Race. |
| `ArrowLog` | `0` | Log the arrow placement for a few seconds after each race start. |
| `AlphaLog` | `0` | Log each blended / alpha-tested draw combination and whether `AlphaCut` applied. |
| `SmoothLog` | `0` | Trace `SimSmooth`: `1` = matrices and draws for a few frames, `2` = camera only (120 frames), `3` = rider shadows, `4` = camera and projection after each race start. |

The shipped `mcm2fix.ini` is generated from the block in
[code/README.md](code/README.md) by `tools/package.sh`, and the parser is
[code/src/config.cpp](code/src/config.cpp).

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
   every size. PNG, JPEG and BMP of any size work, at full 32-bit colour,
   and mipmaps are generated on the GPU. Keep alpha where the original has
   it.

Videos, menu screens, the cursor and fonts aren't textures, so they can't be
replaced. Hashes change with `TrueColorTextures`, so dump again if you
switch it. `tools/sky_tiles.py` stitches a track's sky tiles into one
picture for editing and splits it back.

## Problems and logs

Each run writes `%LOCALAPPDATA%\Motocross Madness 2\logs\mcm2fix-<date>-<time>.log`.
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
release zip as `mcm2fix-release`.

MinGW cross-compile on Linux (the release build, with the video decoder and
XP support):

    sudo apt-get install g++-mingw-w64-i686 cmake zip
    tools/build-ffmpeg.sh ffmpeg-work
    cmake -S code -B build -DCMAKE_TOOLCHAIN_FILE=code/toolchain-mingw32.cmake \
          -DCMAKE_BUILD_TYPE=Release -DMCM2FIX_FFMPEG=$PWD/ffmpeg-work/install
    cmake --build build
    tools/package.sh build/mcm2fix.dll mcm2fix.zip

Visual Studio (no videos, no XP):

    cmake -S code -B build -A Win32
    cmake --build build --config Release

`-DMCM2FIX_TESTS=ON` builds `dd_smoke.exe`, which renders a test scene
through both renderers without the game, and runs under Wine. More detail is
in [code/README.md](code/README.md).

## Repository layout

| Path | Contents |
|---|---|
| `code/src/` | mcm2fix: game patches (`fixes_*.cpp`), frame pacing (`framerate.cpp`, `smooth*.cpp`), profiler (`prof.cpp`), XP shims (`xp_compat.cpp`) |
| `code/src/dd/` | DirectDraw 7 / Direct3D 7 on Direct3D 11 (`backend.cpp`, `device.cpp`) and Direct3D 9 (`backend9.cpp`, `device9.cpp`), GPU text, HUD text, MSAA, texture replacement |
| `code/src/video/` | AVI reader, MS ADPCM decoder, waveOut audio, Indeo 5 through FFmpeg |
| `code/tests/` | renderer smoke test |
| `tools/` | `package.sh` (release zip), `build-ffmpeg.sh`, `mcm2patch.py` (exe patcher), `hlp2html.py` (WinHelp to HTML), `check-xp-imports.py`, `perf_report.py`, `sky_tiles.py` |
| `package/` | player README, `MCM2HELP.html`, HUD fonts, licences |
| `docs/` | [why the game quit on Windows 11](docs/step1-launch-crash.md), [runtime writes](docs/runtime-writes.md), [the D3D11 renderer](docs/step3-renderer.md), [Windows XP](docs/windows-xp.md), [clean-machine release test](docs/clean-machine-test.md) |

## Licences

- The video decoder is [FFmpeg](https://ffmpeg.org) n7.1, built with only its
  Indeo 5 decoder and linked statically, under the GNU LGPL 2.1 or later
  (`licenses/FFmpeg-LGPL-2.1.txt`). `tools/build-ffmpeg.sh` rebuilds it, so
  the DLL can be relinked against a modified FFmpeg.
- The HUD fonts are Contrail One (c) 2011 Sorkin Type Co and DSEG7 Classic
  (c) 2017 keshikan, both under the SIL Open Font License 1.1
  (`licenses/ContrailOne-OFL.txt`, `licenses/DSEG-OFL.txt`).
- Motocross Madness 2 is (c) Microsoft / Rainbow Studios. This repository
  and the release contain no game files.
