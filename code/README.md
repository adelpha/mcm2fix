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
  aspect ratio kept. No display mode change and no exclusive fullscreen
  unless `Fullscreen=Exclusive` asks for it (the picture is still scaled into
  that mode); no d3drm.dll, no Windows DirectDraw for drawing. See `docs/step3-renderer.md`.
- Frame pacing (`src/framerate.cpp`): VSync and frame-cap settings, a precise
  frame timer, and frame-rate independence for the few simulation, rider
  animation and camera steps that the game applied once per frame.
- Videos (intro, credits, menu event previews; Indeo 5 with MS ADPCM sound)
  play through mcm2fix's own player (`src/video/`): the AVI is read and
  decoded with FFmpeg's Indeo 5 decoder, sound goes through waveOut, and no
  DirectShow or Indeo codec is needed. A build without FFmpeg skips the
  videos.

Each run writes its own log, `%LOCALAPPDATA%\Motocross Madness 2\logs\mcm2fix-YYYYMMDD-HHMMSS.log`; the newest 50 are kept.

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

All settings default to the values shown. The file is grouped into preferences,
advanced settings that are best left alone (physics, timing, compatibility),
and logging, where everything but the main log is off. `tools/package.sh`
ships this block as the release `mcm2fix.ini`.

```ini
[mcm2fix]
; mcm2fix settings for Motocross Madness 2.
;
; Every setting is optional: delete a line, or the whole file, and the default
; shown here is used. On/off settings take 1 (on) or 0 (off). Lines starting
; with ';' are comments. Changes take effect the next time the game starts.
;
; The file has three parts:
;   1. PREFERENCES  - change these to taste.
;   2. ADVANCED     - keep the game working and playing correctly; changing
;                     them is NOT recommended.
;   3. LOGGING      - only the main log is on; the rest are for debugging.

; =============================================================================
; 1. PREFERENCES
; =============================================================================

; ---------------------------------------------------------------- Display ----

; Renderer: the graphics API used to draw the game.
;   D3D11 - Direct3D 11 (default, recommended on Windows 7 and later).
;   D3D9  - Direct3D 9, fixed function. Used automatically when Direct3D 11 is
;           missing (Windows XP, very old graphics cards).
Renderer=D3D11

; Fullscreen: how the game fills the screen.
;   Borderless - a borderless window covering the primary monitor at its
;                current resolution (default). The desktop resolution never
;                changes, and Alt+Tab is instant.
;   Exclusive  - the game takes over the screen. Alt+Tab leaves fullscreen and
;                it comes back when you return. Can help VSync on Windows XP.
; Either way, the game renders at the resolution chosen in its own Options
; and the picture is scaled to the screen with its shape kept.
Fullscreen=Borderless

; FullscreenMode: the screen mode used with Fullscreen=Exclusive (ignored when
; Borderless).
;   Desktop                  - keep the current desktop mode (default).
;   <width>x<height>         - e.g. 1920x1080, at the highest refresh rate.
;   <width>x<height>@<Hz>    - e.g. 1024x768@85.
FullscreenMode=Desktop

; VSync: 1 = wait for the monitor's refresh before showing each frame: no
; tearing, frame rate limited to the refresh rate (default).
; 0 = show frames as soon as they are ready: higher frame rate, tearing where
; Windows allows it.
VSync=1

; FrameCap: the most frames per second, on top of VSync. 0 = no cap (default).
; Useful with VSync=0, e.g. FrameCap=120. Does not change game speed.
FrameCap=0

; WideFov: 1 = on widescreen resolutions, the view is widened so you see more
; to the sides (default). 0 = keep the 4:3 view width and crop the top and
; bottom, like the original game.
WideFov=1

; Fov: field of view in degrees, as a 4:3 screen would show it, 20 to 150.
; 0 = the game's own default, 77 (default). WideFov widens it further on
; widescreen; the game's zoom keys still step from this value.
Fov=0

; RendererLabel: 1 = show "Direct3D 11" or "Direct3D 9" in the bottom right
; corner of the menus (default). Only shown while no 3D scene is being drawn.
; 0 = hide it.
RendererLabel=1

; ------------------------------------------------------- Graphics quality ----

; Antialiasing: multisample antialiasing (MSAA) of the 3D scene, which smooths
; jagged edges. 0 or 1 = off, 2, 4 (default) or 8 samples. If the graphics
; card can't do the number asked for, the next lower one is used.
Antialiasing=4

; Anisotropy: anisotropic texture filtering, which keeps the ground sharp at
; shallow angles. 0 or 1 = off, 2 to 16 (default 16). Capped at what the
; graphics card supports.
Anisotropy=16

; MipBias: texture sharpness at a distance, -4 to 4. 0 = as the game sets it
; (default). Negative = sharper but may shimmer (e.g. -0.5); positive = softer.
MipBias=0

; ViewDistance: multiplies how far the game draws, 0.25 to 10 (default 1).
; The far clipping distance, the distance objects such as trees are drawn to,
; and the fog all move out together, so the fog still hides the edge.
; 2 = twice as far. Higher values cost frame rate.
ViewDistance=1

; FogDistance: multiplies the fog distances on top of ViewDistance, 0.1 to 20
; (default 1). 2 = fog starts twice as far away. Above 1, the edge of the
; drawn world may show through the fog unless ViewDistance is raised too.
FogDistance=1

; ModelLod: detailed models are kept this many times further away before the
; game switches to simpler versions, 0.25 to 10 (default 1 = the game's
; distances).
ModelLod=1

; TreeDetailDistance: 1 = trees switch between their dense far-away version
; and their sparse close-up version as the game intends (default).
; 0 = every tree uses its close-up version at any distance (looks sparse; for
; fun only).
TreeDetailDistance=1

; AlphaCut: for trees, flags and other cut-out pictures, pixels less opaque
; than this (0 to 255) are not drawn. This removes the stair-step fringe where
; tree edges hide whatever is behind them, a flaw of the original game.
; 128 = default. 0 = the original look. Soft see-through layers (shadows,
; ground detail, supercross ground) are never affected. With
; AlphaToCoverage on, this is where the smoothed edge sits instead.
AlphaCut=128

; AlphaToCoverage: smooth, antialiased edges on cut-out pictures (tree
; leaves, flags, fences, metal arches, tents, desert plants) by turning their
; transparency into MSAA coverage. Only works while Antialiasing is 2 or more;
; without MSAA, AlphaCut and the game's own alpha test are used as before.
; 2 = soft (default): tree leaves and flags use their transparency as is,
; so distant trees look fuller and fade smoothly between their far-away and
; close-up versions; edges are softer (fences and other solid models are
; always sharpened). 1 = sharpened: crisp edges about one pixel wide at any
; distance, but trees switch abruptly between versions. 0 = off.
; Direct3D 11 only: the Direct3D 9 renderer uses AlphaCut.
AlphaToCoverage=2

; MotionBlur: 1 = speed blur. Above MotionBlurMinSpeed the edges of the 3D
; scene streak towards the point your bike is heading for, more the faster
; you go, while the middle of the screen stays sharp. The HUD, text and menus
; are never blurred. 0 = off (default).
MotionBlur=0

; MotionBlurStrength: length of the streaks at full speed, in percent (higher
; = longer streaks), 0 to 300. Default 80.
MotionBlurStrength=80

; MotionBlurMinSpeed / MotionBlurMaxSpeed: speedometer speeds (mph) where the
; blur starts and where it is at full strength. Defaults 30 and 70.
MotionBlurMinSpeed=30
MotionBlurMaxSpeed=70

; MotionBlurCenter: size of the sharp middle of the screen, in percent of the
; way from the middle to the screen edges, 0 to 95. Only the part outside it
; streaks, most at the edges. Default 60.
MotionBlurCenter=60

; TrueColorTextures: 1 = load textures in full colour (24-bit, or 32-bit with
; alpha) wherever the picture has more than 16 bits of colour (default).
; The game always cut them down to 16-bit because of a bug. Most of the
; game's own art is stored in 16-bit already, so only a few textures change.
; 0 = the game's 16-bit textures. Changing it changes the names of unnamed
; dumped textures (see TextureDump).
TrueColorTextures=1

; TextureReplace: 1 = when the game loads a texture, use a replacement picture
; if there is one (default). Replacements go in the "textures" folder in the
; game folder or in %LOCALAPPDATA%\Motocross Madness 2\textures (that one wins
; when both have the same picture). Name each file like the dumped texture
; (see TextureDump). PNG, JPEG or BMP, any size, full colour with alpha.
; 0 = always use the game's own textures.
TextureReplace=1

; TextureDump: 1 = save every texture the game loads as a PNG in
; %LOCALAPPDATA%\Motocross Madness 2\textures_dump, at the largest size the
; game loaded it, to use as a starting point for replacements. Named textures
; are saved as <name>.png, others as <16 hex digits>.png. 0 = off (default).
; Slows loading while on.
TextureDump=0

; ------------------------------------------------------------ Text and HUD ----

; GpuText: 1 = the game's menu and HUD text is drawn by the graphics card,
; antialiased, at your screen's resolution instead of the game's (default).
; 0 = the game's own text, drawn at the game's resolution and scaled up.
GpuText=1

; HudText: 1 = the race HUD panels (position, lap, times, stunt score) and the
; rider name labels are redrawn sharp by mcm2fix, using HudScale, LabelScale
; and the HUD fonts below (default). Needs GpuText=1. 0 = the game's panels.
HudText=1

; HudScale: size of the race HUD text panels, 1 (original) to 4. Default 2.
HudScale=2

; LabelScale: size of the name labels over the riders, 1 (original) to 4.
; Default 1.5. Names are shrunk to fit if they would be too long.
LabelScale=1.5

; LineWidth: width in pixels of the speedometer needle, 1 (original thin line)
; to 16. Default 3. Drawn antialiased.
LineWidth=3

; HudFont: TrueType font file for the HUD panel and name label text, relative
; to the game folder (or a full path). The font is used by the game only and
; is not installed in Windows. Empty (HudFont=) = the game's own fonts.
HudFont=FONTS\ContrailOne-Regular.ttf

; HudDigitFont: TrueType font file for the numbers in the race stats panel
; (times, position, lap); the words beside them keep HudFont. Any character
; the font lacks falls back to HudFont. Empty = use HudFont for numbers too.
HudDigitFont=FONTS\DSEG7Classic-BoldItalic.ttf

; HudDigitSize: size of those numbers relative to the words beside them,
; 0.5 to 2. Default 0.7.
HudDigitSize=0.7

; HudStuntTextSize: stunt events only: size of the score panel's text relative
; to the race panel's, 0.5 to 1.5. Default 0.8.
HudStuntTextSize=0.8

; RaceTitle: 1 = during the countdown the event's name and lap count (stunt
; events: time limit) open out of a white line in the middle of the screen
; and close back into it as the countdown ends, instead of the game's small
; green name in the bottom-left corner (default). FINAL LAP is shown the same
; way (held 2 seconds) instead of dropping down the screen, and the finish
; message opens near the top of the screen for the length of the podium
; cutscene (top 3; otherwise the game shows it as usual). Needs GpuText=1.
; 0 = the game's corner text and drop text.
RaceTitle=1

; RaceTitleSize: size of that title, 0.5 to 2. Default 1.
RaceTitleSize=1

; Announcer: 1 = play WAV files from the game's SOUNDS\ folder, if they are
; there: five.wav, four.wav, three.wav, two.wav and one.wav as the countdown
; passes 5, 4, 3, 2 and 1 seconds left, airhorn.wav when the race starts,
; finallap.wav when FINAL LAP shows (not in one-lap races) and podium.wav
; when the podium cutscene starts (default). The release ships all eight in
; its SOUNDS\ folder; replace any with your own WAV of the same name.
; Missing files are skipped. 8 or 16-bit PCM WAV. 0 = off.
Announcer=1

; AnnouncerVolume: volume of those sounds in percent, 0 to 100. Default 100.
AnnouncerVolume=100

; --------------------------------------------------- Race direction arrow ----

; ArrowFix: 1 = the green race direction arrow (bottom left) points at the
; next gate (default). 0 = the original arrow, which leans toward the centre
; of the screen because of a perspective bug.
ArrowFix=1

; ArrowTurn: how much of that correction is applied, 0 to 2. Default 0.8.
; 1.0 = the full mathematical correction; lower it if the arrow points to the
; left of a gate straight ahead, raise it if it points to the right.
ArrowTurn=0.8

; ArrowTilt: degrees the arrow's nose is raised so it is seen a little from
; above, -45 to 45. Default 10. 0 = level, as in the original.
ArrowTilt=10

; ArrowScale: size of the arrow, 0.25 to 8. Default 2.0. 1.0 = original size.
ArrowScale=2.0

; ------------------------------------------------------ Computer riders ----

; AiAccelEasy / AiAccelNormal / AiAccelHard: how hard computer riders can
; accelerate on each difficulty, in percent of the game's base curve.
; Defaults 80 / 130 / 150. The original game uses 110 for Easy, which is
; barely easier than Normal (especially in Supercross). Lower = they pull
; away more slowly out of corners and after landings.
AiAccelEasy=80
AiAccelNormal=130
AiAccelHard=150

; AiTopSpeedEasy / AiTopSpeedNormal / AiTopSpeedHard: computer riders' top
; speed on each difficulty, in percent, 50 to 150. Defaults 90 / 100 / 100.
; The original game uses 100 for all three (difficulty never changes top
; speed).
AiTopSpeedEasy=90
AiTopSpeedNormal=100
AiTopSpeedHard=100

; =============================================================================
; 2. ADVANCED - CHANGING THESE IS NOT RECOMMENDED
;
; These settings keep the game starting, saving and playing correctly on
; modern PCs. Changing them can stop the game starting, lose settings or
; saves, make bikes and computer riders behave differently, make motion
; judder, or bring back bugs of the original game. Leave them as they are
; unless you are troubleshooting.
; =============================================================================

; ----------------------------------------------------------- Compatibility ----

; Startup: 1 = no video card picker (the primary display adapter is used) and
; no "detected a change in your video hardware" box (default).
; 0 = the original prompts.
Startup=1

; Registry: 1 = the game's settings are kept in
; HKEY_CURRENT_USER\Software\mcm2fix\Motocross Madness 2 (default). On first
; run, settings from an earlier install in HKLM are copied there.
; 0 = the original HKLM key, which needs administrator rights to write.
Registry=1

; Files: 1 = everything the game writes (profiles, replays, ghosts, tracks,
; circuits, its own .ini and log files) goes to
; %LOCALAPPDATA%\Motocross Madness 2 instead of the game folder (default).
; Files there are read in preference to the game folder's.
; 0 = write into the game folder, which fails under Program Files without
; administrator rights. Saves made with 1 are not seen with 0.
Files=1

; OneCore: 1 = run the game on one CPU core only. An old workaround for a
; startup hang that is now fixed; it costs performance. 0 = all cores
; (default).
OneCore=0

; --------------------------------------------------- Simulation and timing ----

; SimRate: during races, the game's physics and AI run this many steps per
; second whatever the frame rate, 10 to 1000. Default 30.
; The game's physics and AI were tuned at low frame rates and go wrong when
; stepped faster: 30 is the tested value. 0 = one step per drawn frame, the
; original behaviour (bikes and AI misbehave at high frame rates).
; Outside races (menus) the game always steps once per frame.
SimRate=30

; SimSmooth: 1 = with SimRate, each frame shows the camera, bikes, riders,
; shadows and name labels blended between the last two simulation steps, so
; motion is smooth at any refresh rate, with one step (1/30 s) of delay
; (default). 0 = show each step as it is: motion judders at 30 steps a second.
SimSmooth=1

; InputTiming: 1 = with SimRate, each race step gets the controls as they were
; during its own 1/30 s, also when a slow frame makes several steps run at
; once, and a key or button tapped faster than one step still counts for one
; step (default). 0 = the original reading: after a slow frame every catch-up
; step gets the controls as they are at that moment, and quick taps can be
; missed.
InputTiming=1

; AltTabFix: 1 = keys and buttons held when the game goes to the background
; (such as Alt from Alt+Tab) are released, any let go meanwhile count as let
; go when it comes back, and a keyboard or joystick Windows took
; away is acquired again (default; needs InputTiming=1). 0 = the original:
; such keys stay held until pressed again, and the keyboard can stop working
; after alt-tab.
AltTabFix=1

; PreciseTimer: 1 = the game's frame time is measured with the precise
; QueryPerformanceCounter instead of whole milliseconds (default). Only used
; with SimRate=0; with SimRate the precise timer is always used.
PreciseTimer=1

; FrameFix: 1 = the few effects the game applies once per step instead of per
; second (steering and lean speed, spin damping, bodies coming to rest,
; camera zoom) are scaled to the actual step length (default).
; 0 = applied once per step, as in the original. With SimRate=30 and
; RefRate=30 race steps are unchanged; it matters in menus and with SimRate=0.
FrameFix=1

; RefRate: the step rate, 10 to 240, that FrameFix treats those per-step
; effects as tuned for. Default 30.
RefRate=30

; AiRate: computer riders re-plan their racing line at most this many times a
; second. Default 30. 0 = every step. Only has an effect when it is lower
; than SimRate, or with SimRate=0.
AiRate=30

; AiSteerRate: fastest a computer rider's front wheel can turn, in degrees per
; second. 0 = no limit, as in the original (default). Values change how the
; AI rides.
AiSteerRate=0

; AiPickFix: 1 = computer riders' random bike and rider picks can choose the
; last entry in the list (default). 0 = the original off-by-one: the last bike
; and rider are never picked.
AiPickFix=1

; RestartFix: 1 = "Restart Race" in the pause menu puts every rider back on
; their start position facing the course, and FINAL LAP shows again on the
; last lap (default). 0 = the original bugs: riders that had crashed or left
; the track restart in the wrong place or facing the wrong way.
RestartFix=1

; ------------------------------------------------------ Sound and performance ----

; AsyncSound: where the 3D sound position update (made every physics step, and
; slow on Windows 10/11 because it holds the sound system's lock) runs.
;   2 = on a worker thread, started while the frame waits for VSync (default).
;   1 = on a worker thread, started at once.
;   0 = on the game thread, as in the original (can cause stutter).
AsyncSound=2

; SoundMaxDistanceMute: 1 = 3D sounds further away than their maximum distance
; are muted by mcm2fix when the DirectSound in use is a replacement such as
; DSOAL, which ignores that rule and otherwise leaves distant riders' sounds
; playing quietly (default). Windows' own DirectSound mutes them itself, so
; with it mcm2fix leaves them alone. 3 = mute them with any DirectSound (with
; Windows' own this silences the bike engines after a pause). 0 = never.
; 2 = change nothing but log each 3D sound that goes beyond its maximum
; distance, and how often the game restarts a sound that is still playing.
SoundMaxDistanceMute=1

; BackgroundMute: 1 = with a replacement DirectSound such as DSOAL, silence
; the game while it is in the background (default; Windows' own DirectSound
; does this itself). 0 = leave the sound alone: the game stops while in the
; background and its sounds keep repeating their last fraction of a second.
BackgroundMute=1

; BajaReverb, EnduroReverb, QuarryReverb, NationalsReverb, SupercrossReverb,
; TagReverb: the EAX 1.0 reverb preset used in each kind of event. The game
; turns reverb off for every outdoor event, but with values (decay time 0) that
; OpenAL Soft / DSOAL rejects, so whatever reverb was set before stayed on.
; Supercross and Moto-Tag use the game's own Arena reverb unless
; SupercrossReverb or TagReverb names a preset. Presets: Generic, PaddedCell,
; Room, Bathroom, LivingRoom, StoneRoom, Auditorium, ConcertHall, Cave, Arena,
; Hangar, CarpetedHallway, Hallway, StoneCorridor, Alley, Forest, City,
; Mountains, Quarry, Plain, ParkingLot, SewerPipe, Underwater, Drugged, Dizzy,
; Psychotic. Default = the game's own values for that event (Arena in
; Supercross and Moto-Tag). Needs EAX (DSOAL or an EAX sound card); otherwise
; they change nothing. Every EAX setting the game makes is written to the log.
BajaReverb=Mountains
EnduroReverb=Forest
QuarryReverb=Mountains
NationalsReverb=Forest
SupercrossReverb=Default
TagReverb=Default

; FastCrt: 1 = the game's millisecond clock uses QueryPerformanceCounter
; instead of changing the Windows timer period on every call, and its
; case-insensitive name comparisons use a lookup table (default). Both were
; measurable slowdowns. 0 = the game's own code.
FastCrt=1

; Simd: 1 = mcm2fix's own per-frame work (converting vertices for the GPU,
; blending objects between simulation steps, 16-bit texture uploads, the
; pause menu's copy of the frame) uses SSE2 when the CPU has it (default).
; The CPU is checked at start-up and a self test makes sure both versions
; give the same results. 0 = plain code only. The game's own code, physics
; included, is not affected either way.
Simd=1

; SimdLog: 1 = log how fast the SSE2 and plain versions run on this CPU at
; start-up, and how much work went through them every 5 seconds.
SimdLog=0

; PauseOnFocusLoss: 1 = switching away from the game mid-race opens the pause
; menu, as Escape does (default). 0 = the race isn't paused (the game still
; stops while in the background, and its clock with it).
PauseOnFocusLoss=1

; DpiAware: 1 = tell Windows the game handles display scaling itself, so with
; scaling above 100% it runs at the screen's real resolution instead of being
; stretched (blurry, with a drifting menu mouse) (default). 0 = Windows
; scales the game as before.
DpiAware=1

; RacePower: keeps the CPU at full speed while racing. The game needs so
; little CPU that on the Balanced power plan Windows lowers the clock while
; it waits for VSync, and some frames then run late (regular stutter,
; mostly on older CPUs). 1 = switch Windows to the High performance power
; plan when a race starts and back to your plan after it (also put right at
; the next start if the game crashed). 2 = High performance the whole time
; the game runs. 0 = off. Nothing is changed on battery or when the plan is
; already High performance.
RacePower=1

; FastGameCode: 1 = a few of the game's own loops that its 1999 compiler
; built badly are rewritten in place with the same maths, so the results are
; exactly the same, only faster (default). So far: the loop that unpacks
; terrain texture coordinates every frame, and the per-frame tree cache scan
; (reads ahead instead of waiting on memory). 0 = the game's own code.
FastGameCode=1

; FastSort: 1 = the game's sorting uses a merge sort instead of the old C
; library qsort, which slows to a crawl when many items are equal (default).
; On maps with many trees that froze the game for 35-125 ms when turning the
; camera. The merge sort is also stable, which stops distant riders' engine
; sounds from retriggering as quiet looping snippets (the game re-sorts its
; engine voices every update and qsort shuffled equal entries). 0 = the
; game's own qsort.
FastSort=1

; TreeCacheMB: the game keeps built trees in a cache and empties the unused
; part whenever more than 256 KB of it is unused, which happens constantly
; while turning on tree-heavy maps (and the trees are then built again when
; they come back into view). It is now only emptied when more than this many
; MB are unused, 1 to 512. Default 32. 0 = the original 256 KB.
TreeCacheMB=32

; StateCache: Direct3D 11 only. 1 = renderer state that is already set is not
; sent to the graphics card again for every draw (default). 0 = send it every
; draw (slower; for troubleshooting rendering problems).
StateCache=1

; DrawBatch: 1 = 3D draws in a row with nothing changed between them (trees,
; terrain, scenery) are sent to the graphics card as one draw (default). 0 =
; send each draw the game makes on its own (for troubleshooting).
DrawBatch=1

; =============================================================================
; 3. LOGGING
;
; Log=1 writes one short log per run; please include the newest one with any
; problem report. Everything else in this part is for debugging: it slows the
; game down and makes the log much bigger, so turn one on only when asked to.
; Logs go to %LOCALAPPDATA%\Motocross Madness 2\logs.
; =============================================================================

; Log: 1 = write logs\mcm2fix-<date>-<time>.log each run; the newest 50 are
; kept (default). It records the Windows version, these settings, the
; renderer chosen and any problems. If the game crashes it also records where
; and why, and writes a crash dump (mcm2fix-<date>-<time>.dmp) next to the
; log. 0 = no log.
Log=1

; D3D9Log: Direct3D 9 renderer details: card capabilities, the first frames,
; fallbacks. Default 0.
D3D9Log=0

; AiLog: computer rider speeds every 5 s, plus every jump (air time, speed at
; takeoff and landing) and respawn, for computer riders and the player.
; Default 0.
AiLog=0

; DDStats: renderer activity (draws, uploads, surfaces) every 2 seconds.
; Default 0.
DDStats=0

; Profile: performance profile. Frame times, timed sections, GPU time and a
; sampling profile of the game thread go to logs\perf-*.csv and
; logs\perf-samples-*.txt (summarise with tools/perf_report.py from the
; source). Costs some frame rate. Default 0.
Profile=0

; ProfileSampleUs: with Profile=1, how often the game thread is sampled, in
; microseconds. Default 1000. 0 = no sampling (timings only).
ProfileSampleUs=1000

; ActivateLog: window activation (the game pauses while it is in the
; background). Default 0.
ActivateLog=0

; RegLog: every registry call the game makes. Default 0.
RegLog=0

; TextLog: GPU text drawing and what erases it. Default 0.
TextLog=0

; BatchLog: every 5 seconds, how many draws DrawBatch merged and what ended
; each batch. Default 0.
BatchLog=0

; DrawLog: during races, which draw calls could be merged into fewer (per game
; call site, plus a few whole frames listed draw by draw). Costs about 1.5 ms
; a frame. Default 0.
DrawLog=0

; GdiLog: the GDI calls (text, boxes, lines) the game draws onto the screen
; with. Default 0.
GdiLog=0

; TextureLog: the format of each texture the game creates. Default 0.
TextureLog=0

; InputLog: with InputTiming, the keyboard and joystick input each race step
; gets: events given out early or held for a later step, held taps, joystick
; samples, plus a summary every 10 s. Default 0.
InputLog=0

; SortLog: the game's sorts per calling code every 10 s (count, largest,
; time). Sorts over 2 ms are always logged. Default 0.
SortLog=0

; SoundLog: 3D sound buffers, and when they are muted or unmuted by
; SoundMaxDistanceMute. Default 0.
SoundLog=0

; SoundPauseLog: when a race is paused and unpaused, every DirectSound call for
; a few seconds, any call that fails, and the state of every sound buffer and
; of the game window DirectSound follows. Default 0.
SoundPauseLog=0

; RaceTitleLog: the countdown race title: name and lap count found, timing
; and placement. Default 0.
RaceTitleLog=0

; RestartLog: the start grid when a race loads and on Restart Race. Default 0.
RestartLog=0

; ArrowLog: the direction arrow's placement for a few seconds after each race
; start. Default 0.
ArrowLog=0

; FovLog: every change to a camera's field of view: who set it, what mcm2fix
; widened it to for the screen shape and the smoothing cull margin. Default 0.
FovLog=0

; AlphaLog: each kind of see-through draw and whether AlphaCut or
; AlphaToCoverage applied to it, plus alpha-to-coverage counts every 10 s.
; Default 0.
AlphaLog=0

; MotionBlurLog: with MotionBlur, every 5 s: speed, blur amount and focus
; point, where in the frame the blur went, and a few whole-frame draw lists.
; Default 0.
MotionBlurLog=0

; SmoothLog: SimSmooth tracing. 0 = off (default), 1 = matrices and draws for
; a few frames, 2 = camera only (120 frames), 3 = rider shadows, 4 = camera and
; projection after each race start.
SmoothLog=0
```

## True-colour textures

The game converts every picture it loads into a texture format of its own
choosing, and it always picked a 16-bit one: 4444 (16 levels per channel)
for pictures with alpha such as trees, smoke and the HUD, and 555/565 for
the rest, including the 8-bit paletted terrain. It also describes its own
32-bit format wrongly (as 24 bits per pixel), so no driver ever matched it.
`TrueColorTextures=1` fixes that description (0x5170A0), offers 24-bit and
32-bit texture formats, and puts a true-colour format first in the list the
loader tries (0x4CB030), but only where the picture in memory holds more
than 16 bits: 32-bit pictures asked for as 4444/1555 become 32-bit ARGB,
24/32-bit pictures asked for as 565/555 become 24-bit RGB. Most opaque
pictures are already cut down to 555 when the TGA is read, so they stay
16-bit (nothing to gain). Off-screen display-format surfaces and textures
whose caller copies the pixels in itself (0x4CA7D0, which works out the
row pitch from the 16-bit format) are left alone.
Texture hashes change with it, so replacements dumped with it off need
dumping again.

## Texture replacements

Most textures have a name in the game's files (the .RES archives or TGA
files). Set `TextureDump=1`, play, and every named texture is saved as
`%LOCALAPPDATA%\Motocross Madness 2\textures_dump\<name>.png`, at the
largest size the game loaded it (it streams smaller stored versions for
distant objects). The name is lower case, without extension, with folder
separators turned into `_`. Edit or upscale one, keep the file name, and put
it in `textures\` in the game folder or in
`%LOCALAPPDATA%\Motocross Madness 2\textures\`: it replaces that texture
at every size. Textures without a name are dumped as `<hash>.png` instead
(16 hex digits of the texture's size and pixels; anything after the 16
digits and a `_` is ignored) and replaced only when the game loads exactly
those pixels. PNG, JPEG and BMP of any size work, at full 32-bit colour;
mipmaps are generated on the GPU. Keep alpha where the original has it.
Textures that change every frame (videos) are never replaced, and menu
screens, the cursor and fonts are not textures, so they can't be replaced.

Sky boxes are 48 overlapping tiles each. `tools/mcm2sky` (mcm2sky.exe, built
by CI) extracts every sky straight from `RES\SKIES.RES` as one picture and
cuts an edited picture back into correctly named tiles; see
`tools/mcm2sky/README.md`.
