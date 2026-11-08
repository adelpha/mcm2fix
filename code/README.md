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
- Videos (intro, credits, menu event previews; Indeo 5 with MS ADPCM sound)
  play through mcm2fix's own player (`src/video/`): the AVI is read and
  decoded with FFmpeg's Indeo 5 decoder, sound goes through waveOut, and no
  DirectShow or Indeo codec is needed. A build without FFmpeg skips the
  videos.

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
`MCM2FIX_FFMPEG` (e.g. the Visual Studio build) the videos are skipped.

## Installing

The game imports `d3drm.dll`, which Windows no longer ships, and Windows
looks for it in the game folder first. So mcm2fix installs as a drop-in:
copy the DLL into the game folder as `d3drm.dll`, with `mcm2fix.ini`, and
start the unmodified `Mcm2.exe`. `tools/package.sh` builds the release zip
that way (with `package/README.txt` for players).

Alternatively, `python tools/mcm2patch.py Mcm2.exe Mcm2_fixed.exe` makes a
copy of the exe that imports `mcm2fix.dll` by that name instead.

`docs/clean-machine-test.md` is the check to run on a fresh Windows 11
install before a release.

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
SimSmooth=1    ; draw the camera, bikes, riders, shadows and names blended between simulation steps
               ; (smooth motion at any frame rate, one step of delay); 0 = show each step as it is
Fov=0          ; field of view in degrees as a 4:3 screen shows it (0 = game default, 77);
               ; the zoom keys still step from it, widescreen adds to the sides
Anisotropy=16  ; anisotropic texture filtering, 0/1 = off, 2-16
Antialiasing=4 ; multisample antialiasing of the 3D scene: 0/1 = off, 2, 4 or 8
MipBias=0      ; texture sharpness at a distance: negative = sharper (e.g. -0.5), positive = softer
ViewDistance=1 ; multiplies how far the game draws (far clip, culling and fog move together)
FogDistance=1  ; multiplies the fog distances on top of that (2 = fog starts twice as far away)
TreeDetailDistance=1 ; 0 = every tree uses its sparse close-up version at any distance (looks bad; for fun)
ModelLod=1     ; detailed models stay this many times further away before switching to simpler ones
TextureReplace=1 ; draw pictures from textures\<hash>.png in place of the game's textures
TextureDump=0  ; save every texture the game loads to %LOCALAPPDATA%\Motocross Madness 2\textures_dump
ArrowFix=1     ; race direction arrow points the right way (0 = original, leans toward the screen centre)
ArrowTurn=0.8  ; how much of that correction to apply (1.0 = full; lower if the arrow points left of a gate dead ahead)
ArrowTilt=10   ; degrees the arrow's nose is raised so it is seen a little from above (0 = level)
ArrowScale=2.0 ; size of the race direction arrow (1.0 = original)
RestartFix=1   ; Restart Race puts every rider back on the grid facing the course (0 = original bug)
OneCore=0      ; 1 = pin the game to one CPU core (old stopgap)
ActivateLog=0  ; debugging: log window activation
RegLog=0       ; debugging: log every registry call
DDStats=0      ; debugging: log renderer activity every 2 seconds
RestartLog=0   ; debugging: log the start grid at race load and on Restart Race
```

## Texture replacements

Set `TextureDump=1`, play, and every texture the game loads is saved once
as `%LOCALAPPDATA%\Motocross Madness 2\textures_dump\<hash>.png` (the hash
is 16 hex digits of the texture's size and pixels). Edit or upscale one,
keep the file name (anything after the 16 digits and a `_` is ignored), and
put it in `textures\` in the game folder or in
`%LOCALAPPDATA%\Motocross Madness 2\textures\`. PNG, JPEG and BMP of any
size work; mipmaps are generated on the GPU. Keep alpha where the original
has it. Textures that change every frame (videos) are never replaced.
