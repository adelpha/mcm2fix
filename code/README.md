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
- Videos (intro, credits, menu event previews; Indeo 5 with MS ADPCM sound)
  play through mcm2fix's own player (`src/video/`): the AVI is read and
  decoded with FFmpeg's Indeo 5 decoder, sound goes through waveOut, and no
  DirectShow or Indeo codec is needed. A build without FFmpeg falls back to
  the game's DirectShow path (`src/dd/hook.cpp` keeps that working).

The log is `%LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log`.

## Building

32-bit only.

Visual Studio:

    cmake -S code -B build -A Win32
    cmake --build build --config Release

MinGW cross-compile (Linux), with the video decoder:

    tools/build-ffmpeg.sh ffmpeg-work
    cmake -S code -B build -DCMAKE_TOOLCHAIN_FILE=code/toolchain-mingw32.cmake -DCMAKE_BUILD_TYPE=Release -DMCM2FIX_FFMPEG=$PWD/ffmpeg-work/install
    cmake --build build

`tools/build-ffmpeg.sh` builds a static libavcodec with only the Indeo 5
decoder. FFmpeg is licensed under the LGPL 2.1 or later; its source is the
tagged FFmpeg release the script downloads, and mcm2fix's own source is
here, so the DLL can be rebuilt against a modified FFmpeg. Without
`MCM2FIX_FFMPEG` (e.g. the Visual Studio build) the videos use DirectShow.

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
ArrowFix=1     ; race direction arrow points the right way (0 = original, leans toward the screen centre)
ArrowTurn=0.8  ; how much of that correction to apply (1.0 = full; lower if the arrow points left of a gate dead ahead)
ArrowTilt=10   ; degrees the arrow's nose is raised so it is seen a little from above (0 = level)
ArrowScale=2.0 ; size of the race direction arrow (1.0 = original)
OneCore=0      ; 1 = pin the game to one CPU core (old stopgap)
ActivateLog=0  ; debugging: log window activation
RegLog=0       ; debugging: log every registry call
DDStats=0      ; debugging: log renderer activity every 2 seconds
```
