# mcm2fix.dll

Loaded by `Mcm2.exe` in place of `d3drm.dll` (see `tools/mcm2patch.py`).
It provides `D3DRMVectorRotate`, the only function the game used from
d3drm, and patches the game's code in memory from `DllMain`, before the
game's entry point runs. It does nothing unless it finds the supported exe
(image base 0x400000, entry 0x13d6f5, image size 0x296000).

What it does:
- Startup (`src/fixes_step1.cpp`, `src/fixes_startup.cpp`): retries `Flip`
  on `DDERR_SURFACELOST`, skips the administrator probe, auto-picks the
  video card and skips the "hardware changed" box.
- Per-user writes (`src/fixes_registry.cpp`, `src/fixes_files.cpp`): settings
  in `HKCU\Software\mcm2fix\Motocross Madness 2`, saved files in
  `%LOCALAPPDATA%\Motocross Madness 2`. Nothing is written to HKLM or the
  game folder.
- Renderer (`src/dd/`): the game's `DirectDrawCreateEx` import is pointed at
  mcm2fix's own DirectDraw 7 / Direct3D 7 implementation on Direct3D 11. The
  game keeps its 640x480 / 1280x1024 (etc.) modes virtually; the real window
  stays borderless at desktop resolution and the picture is scaled with the
  aspect ratio kept. No display mode change, no exclusive fullscreen, no
  d3drm.dll, no Windows DirectDraw for drawing. See `docs/step3-renderer.md`.
- Frame pacing (`src/framerate.cpp`): VSync and frame-cap settings, a precise
  frame timer, and frame-rate independence for the few simulation, rider
  animation and camera steps that the game applied once per frame.
- Intro videos still play through Windows' DirectShow (amstream) and the
  Indeo codec; `src/dd/hook.cpp` makes that path work with the new renderer.

The log is `%LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log`.

## Building

32-bit only.

Visual Studio:

    cmake -S code -B build -A Win32
    cmake --build build --config Release

MinGW cross-compile (Linux):

    cmake -S code -B build -DCMAKE_TOOLCHAIN_FILE=code/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release
    cmake --build build

## Installing

    python tools/mcm2patch.py Mcm2.exe Mcm2_fixed.exe

Put `Mcm2_fixed.exe` and `mcm2fix.dll` in the game folder.

## mcm2fix.ini (optional, next to Mcm2.exe)

All settings default to the values shown; set one to 0 or 1 to change it.

```ini
[mcm2fix]
Log=1          ; write %LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log
Startup=1      ; auto-pick the video card, skip the "detected a change" box
Registry=1     ; keep settings in HKCU instead of HKLM
Files=1        ; send file writes to %LOCALAPPDATA%\Motocross Madness 2
AlphaCut=128   ; trees and flags: drop pixels less opaque than this (0-255), which
               ; removes the fringe where trees hide flags behind them (0 = original look)
WideFov=1      ; widescreen modes show more to the sides (0 = crop top and bottom like the original)
VSync=1        ; wait for the monitor's refresh (0 = no wait; tears if the system allows it)
FrameCap=0     ; most frames per second (0 = no cap beyond VSync)
PreciseTimer=1 ; the game's frame time from QueryPerformanceCounter instead of whole milliseconds
FrameFix=1     ; steps the game applies once per frame scale with frame time (0 = original)
RefRate=30     ; the frame rate those per-frame steps are treated as tuned for
AiRate=30      ; computer riders re-plan their line this many times a second (0 = every frame)
AiSteerRate=0  ; fastest a computer rider's front wheel can turn, degrees per second (0 = no limit)
SimRate=30     ; races run this many simulation steps per second whatever the frame rate
               ; (0 = one step per frame, the original way; 10-1000)
SimSmooth=1    ; draw objects and camera blended between simulation steps (smooth motion,
               ; one step of delay); 0 = show each step as it is
OneCore=0      ; 1 = pin the game to one CPU core (old stopgap)
ActivateLog=0  ; debugging: log window activation
RegLog=0       ; debugging: log every registry call
DDStats=0      ; debugging: log renderer activity every 2 seconds
```
