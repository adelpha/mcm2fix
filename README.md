# mcm2fix: Motocross Madness 2 for Windows 10 and 11

mcm2fix makes **Motocross Madness 2** (Microsoft / Rainbow Studios, 2000) run
on modern Windows by double-clicking `Mcm2.exe`. You don't need compatibility
settings, administrator rights, DirectX wrappers or old codecs, and you don't
change the game's display resolution.

> **You need the no-CD `Mcm2.exe`.** mcm2fix only works with the widely
> available no-CD exe (1,577,005 bytes; see [Requirements](#requirements)).
> The original `Mcm2.exe` from the CD uses SafeDisc copy protection, which
> Windows 10 and 11 can't run at all, and mcm2fix doesn't patch it. With any
> other exe mcm2fix does nothing.

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
- [Not yet tested](#not-yet-tested)
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
| Trees and flags show a stair-step fringe that hides things behind them. | Fixed by alpha-test threshold (`AlphaCut`) or, with MSAA, alpha-to-coverage (`AlphaToCoverage`), and transparent texels no longer bleed grey into leaf edges. |
| Texture formats are always 16-bit, because of a bug in the game's 32-bit format description. | Fixed (`TrueColorTextures`), plus full 32-bit texture replacements. |
| Distant riders' engines play quiet looping snippets of old sounds. The game re-sorts its engine voices on every update with the C library's qsort, which is unstable: riders with equal priority swap places, so their voices are handed around and restarted. | The game's sorts use a stable merge sort (`FastSort`), so voices stay with their rider. |
| DSOAL ignores the game's "mute at max distance" flag, so riders out of earshot can still be heard quietly. | With a replacement DirectSound such as DSOAL, mcm2fix mutes 3D sounds past their max distance itself (`SoundMaxDistanceMute`). |

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
  default 4x), with **alpha-to-coverage** so tree leaves, fences and other
  cut-outs get smooth edges too (`AlphaToCoverage`).
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
- A built-in profiler for finding hitches (`Profile`).

### Other
- **Easier Easy**: on Easy, computer riders accelerate less hard and have a
  slightly lower top speed than in the original, where Easy was barely easier
  than Normal. Tunable per difficulty (`AiAccel*`, `AiTopSpeed*`).
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
- **A Pentium II / Pentium Pro, AMD Athlon / Duron or newer CPU** (any CPU
  with the CMOV instruction). The original Pentium, Pentium MMX and AMD K6
  series (K6-2, K6-III) are not supported; on machines that old, run the
  original game. No MMX or SSE is needed.
- Nothing else. You don't need the DirectX 7 runtime, `d3drm.dll` from an
  old Windows, the Indeo codec or compatibility settings. If you set
  compatibility modes on `Mcm2.exe` earlier, it's best to remove them.

## Not yet tested

These work in principle but have had little or no testing so far. Reports
are welcome.

- **Windows XP support**: barely tested.
- **4:3, 16:10, 21:9, 32:9 and 64:27 aspect ratios** with the new HUD, text
  and other screen layout changes: not tested. Testing so far has been at
  16:9.

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
   | `SOUNDS\` | the race announcer sounds |
   | `README.txt` | short player notes |
   | `README.md` | this document |
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

Delete `d3drm.dll`, `mcm2fix.ini`, `MCM2HELP.html`, `FONTS\`, `README.txt`,
`README.md`, `licenses\` and `SOUNDS\` from the game folder. To also remove your settings and saves,
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
startup and applies its reverb: Arena in Supercross and Moto-Tag. mcm2fix
works with or without DSOAL.

For every outdoor event the game asks for "reverb off" with a decay time of
0, which OpenAL Soft rejects, so the reverb from before stayed on instead.
mcm2fix sends a real outdoor preset instead: Mountains for Baja and Stunt
Quarry, Forest for Enduro and Nationals. `BajaReverb`, `EnduroReverb`,
`QuarryReverb` and `NationalsReverb` take any EAX 1.0 preset name, or `Off`
for the game's own values. `SupercrossReverb` and `TagReverb` can replace
the game's Arena reverb in Supercross and Moto-Tag too; both are `Off` (the
game's Arena) by default.

DSOAL ignores the `DSBCAPS_MUTE3DATMAXDISTANCE` flag that the game sets on
all its 3D sounds, so riders who are out of earshot can still be heard
quietly. `SoundMaxDistanceMute=1` (the default) has
mcm2fix mute 3D sounds beyond their max distance itself, as Windows'
DirectSound does, when the game is using a replacement DirectSound. With
Windows' own DirectSound mcm2fix leaves this to Windows: muting on top of it
made the bike engines go silent after every pause. The game's own sounds fade
to -20 dB at max distance and then cut off, so a slight snap-off at the edge
is original behaviour.

The quiet looping snippets of old sounds from distant riders, which first
looked like a DSOAL problem, came from the game itself and happen with any
DirectSound. The game re-sorts its engine voices on every update with the C
library's qsort, which doesn't keep equal entries in order, so voices were
handed between riders and restarted. `FastSort=1` (the default) replaces
that sort with a stable one and stops it.

## mcm2fix.ini reference

`mcm2fix.ini` sits next to `Mcm2.exe`. Its settings go in a section called
`[mcm2fix]`. Every setting is optional, and a missing one (or a missing file)
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

### 1. Preferences

#### Display

| Setting | Default | Values | Description |
|---|---|---|---|
| `Renderer` | `D3D11` | `D3D11`, `D3D9` | The graphics API. D3D9 is a fixed-function Direct3D 9 renderer, and it's used automatically when Direct3D 11 is missing (Windows XP, very old cards). |
| `Fullscreen` | `Borderless` | `Borderless`, `Exclusive` | *Borderless*: a borderless window covering the primary monitor at its current resolution. The desktop resolution never changes and Alt+Tab is instant. *Exclusive*: the game takes over the screen, Alt+Tab leaves fullscreen and it comes back when you return. This can help VSync on XP. Either way, the game renders at the resolution chosen in its own Options, scaled to the screen with its shape kept. |
| `FullscreenMode` | `Desktop` | `Desktop`, `<w>x<h>`, `<w>x<h>@<Hz>` | With `Fullscreen=Exclusive` only: the screen mode. `Desktop` keeps the current mode. Without `@<Hz>`, the highest refresh rate is used. Example: `1024x768@85`. |
| `VSync` | `1` | `0`, `1` | `1` waits for the monitor's refresh before showing each frame: no tearing, and the frame rate is limited to the refresh rate. `0` shows frames as soon as they are ready, with tearing where Windows allows it. |
| `FrameCap` | `0` | `0`, frames/s | The most frames per second, on top of VSync. `0` = no cap. Useful with `VSync=0`. It does not change game speed. |
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
| `ModelLod` | `1` | 0.25-10 | Detailed models stay this many times further away before the game switches to simpler versions. |
| `TreeDetailDistance` | `1` | `0`, `1` | `0` makes every tree use its sparse close-up version at any distance. It looks sparse and is for fun only. |
| `AlphaCut` | `128` | 0-255 | For trees, flags and other cut-out pictures, pixels less opaque than this aren't drawn. This removes the stair-step fringe where tree edges hide what's behind them, a flaw of the original. `0` = the original look. Soft see-through layers (shadows, ground detail, supercross ground) are never affected. With `AlphaToCoverage` on, this is where the smoothed edge sits instead. |
| `AlphaToCoverage` | `2` | `0`, `1`, `2` | Smooth, antialiased edges on cut-out pictures (tree leaves, flags, fences, metal arches, tents, desert plants) by turning their transparency into MSAA coverage. Only works while `Antialiasing` is 2 or more. `2` = soft: leaves and flags use their transparency as is, so distant trees look fuller and fade smoothly between their far-away and close-up versions; edges are softer (fences and solid models are always sharpened). `1` = sharpened: crisp edges about one pixel wide, but trees switch abruptly between versions. `0` = off. Direct3D 11 only; the Direct3D 9 renderer uses `AlphaCut`. |
| `TrueColorTextures` | `1` | `0`, `1` | Loads textures in full colour (24-bit, or 32-bit with alpha) wherever the picture has more than 16 bits; a bug made the game always use 16-bit. Most of the game's art is stored in 16-bit, so only a few textures change. Changing it renames unnamed dumped textures. |
| `TextureReplace` | `1` | `0`, `1` | Uses replacement pictures from `textures\` in the game folder or in `%LOCALAPPDATA%\Motocross Madness 2\textures\` (the per-user folder wins). See [Texture dumping and replacement](#texture-dumping-and-replacement). |
| `TextureDump` | `0` | `0`, `1` | Saves every texture the game loads as a PNG in `%LOCALAPPDATA%\Motocross Madness 2\textures_dump`, to start replacements from. Loading is slower while it's on. |

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
| `Registry` | `1` | `0`, `1` | The game's settings are kept in `HKCU\Software\mcm2fix\Motocross Madness 2`; on first run, settings from an earlier install in HKLM are copied there. `0` = the original HKLM key, which needs administrator rights. |
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
| `AiSteerRate` | `0` | `0`, degrees/s | The fastest a computer rider's front wheel can turn (`0` = no limit, as in the original). Other values change how the AI rides. |
| `RestartFix` | `1` | `0`, `1` | Restart Race puts every rider back on their start position facing the course, and FINAL LAP shows again. `0` = the original bugs: riders who had crashed or left the track restart in the wrong place or facing the wrong way. |

#### Sound and performance

| Setting | Default | Values | Description |
|---|---|---|---|
| `AsyncSound` | `2` | `0`, `1`, `2` | Where the 3D sound position update runs. The game makes it every physics step, and it's slow on Windows 10/11 because it holds the sound system's lock. `2` = a worker thread, started while the frame waits for VSync. `1` = a worker thread, started at once. `0` = the game thread, as in the original (can cause stutter). |
| `SoundMaxDistanceMute` | `1` | `0`, `1`, `2`, `3` | `1`: with a replacement DirectSound such as DSOAL, which ignores the rule, mcm2fix mutes 3D sounds beyond their maximum distance itself, as Windows' DirectSound does. With Windows' own DirectSound it leaves them to Windows. `3` mutes them with any DirectSound (with Windows' own, the bike engines then go silent after a pause). `0` never. `2` changes nothing and only logs those sounds. |
| `FastCrt` | `1` | `0`, `1` | The game's millisecond clock uses `QueryPerformanceCounter` instead of changing the Windows timer period on every call, and its case-insensitive name comparisons use a lookup table. Both were measurable slowdowns. |
| `FastGameCode` | `1` | `0`, `1` | A few of the game's own loops that its 1999 compiler built badly are rewritten in place with the same maths, so results are exactly the same, only faster. So far: the terrain texture coordinate loop, which ran every frame and stalled the CPU, and the per-frame tree cache scan, which now reads ahead instead of waiting on memory. |
| `FastSort` | `1` | `0`, `1` | The game's sorting uses a merge sort instead of the old C library qsort, which slows to a crawl when many items are equal. On tree-heavy maps that froze the game for 35-125 ms when turning the camera. Being stable, it also stops distant riders' engine sounds retriggering as quiet looping snippets. |
| `TreeCacheMB` | `32` | `0`, 1-512 | The game's cache of built trees is only emptied when more than this many MB of it are unused (the original 256 KB emptied it constantly while turning, and the trees were then rebuilt). `0` = the original. |
| `StateCache` | `1` | `0`, `1` | Direct3D 11 only: renderer state that's already set isn't sent again for every draw. `0` is slower and is for troubleshooting rendering problems. |
| `DrawBatch` | `1` | `0`, `1` | 3D draws in a row with nothing changed between them (trees, terrain, scenery) are sent as one draw. `0` sends each draw on its own, for troubleshooting. |

### 3. Logging

`Log=1` writes one short log per run; please include the newest one with any
problem report. Everything else here is for debugging: it slows the game down
and makes the log much bigger. All of it is off by default, so turn one on
only when asked to. Logs go to `%LOCALAPPDATA%\Motocross Madness 2\logs`.

| Setting | Default | Description |
|---|---|---|
| `Log` | `1` | Writes `logs\mcm2fix-<date>-<time>.log` each run (the newest 50 are kept): Windows version, these settings, the renderer chosen and any problems. If the game crashes it also records where and why, and writes a crash dump (`.dmp`) next to the log. `0` = no log. |
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
| `SortLog` | `0` | The game's sorts per calling code every 10 s (count, largest, time). Sorts over 2 ms are always logged. |
| `SoundLog` | `0` | 3D sound buffers, and when `SoundMaxDistanceMute` mutes or unmutes them. |
| `SoundPauseLog` | `0` | When a race is paused and unpaused: every DirectSound call for a few seconds, failed calls, and the state of every sound buffer and of the window DirectSound follows. |
| `RaceTitleLog` | `0` | The countdown race title: name and lap count found, timing and placement. |
| `RestartLog` | `0` | The start grid when a race loads and on Restart Race. |
| `ArrowLog` | `0` | The direction arrow's placement for a few seconds after each race start. |
| `FovLog` | `0` | Every change to a camera's field of view: who set it and what mcm2fix widened it to. |
| `AlphaLog` | `0` | Each kind of see-through draw, and whether `AlphaCut` or `AlphaToCoverage` applied to it, plus alpha-to-coverage counts every 10 seconds. |
| `SmoothLog` | `0` | `SimSmooth` tracing: `1` = matrices and draws for a few frames, `2` = camera only (120 frames), `3` = rider shadows, `4` = camera and projection after each race start. |

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
