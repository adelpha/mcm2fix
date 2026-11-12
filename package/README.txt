Motocross Madness 2 fix (mcm2fix)
=================================

Makes Motocross Madness 2 run on Windows 10 and 11 by double-clicking
Mcm2.exe: no compatibility settings, no administrator rights, no DirectX
wrappers.

- Draws with Direct3D 11 in a borderless window at your desktop
  resolution. The display mode never changes and there is no adapter picker.
  Fullscreen=Exclusive in mcm2fix.ini gives true exclusive fullscreen
  instead, optionally in another mode (FullscreenMode).
- Supports widescreen resolutions, with a wider field of view to match.
- Plays the intro, credits and menu videos itself, so the Indeo codec and
  DirectShow are not needed.
- Keeps settings in your user registry (HKCU) and saves in
  %LOCALAPPDATA%\Motocross Madness 2. Nothing is written to the game folder
  or HKLM.
- The in-game Help buttons open MCM2HELP.html, a web page converted from
  the game's MCM2HELP.HLP (Windows 10 and 11 cannot open .HLP files). It
  opens in your web browser and keeps the original pages, pictures, stunt
  videos, index and search.
- Draws menu and race HUD text sharp at your screen resolution. The HUD
  uses the fonts in the FONTS folder (loaded by the game only, nothing is
  installed in Windows); HudFont and HudDigitFont in mcm2fix.ini choose
  others, or empty for the game's own.
- A race announcer: countdown voices, an air horn at the start, a final
  lap call and a podium sound (the .wav files in the SOUNDS folder;
  Announcer=0 in mcm2fix.ini turns it off).
- Smooth frame pacing at any refresh rate, plus graphics options
  (antialiasing, anisotropic filtering, view distance, FOV) and fixes for the
  race direction arrow and Restart Race.

Requirements
------------
- The game files installed in a folder, with this Mcm2.exe:
    size 1,577,005 bytes
    SHA-256 345e4f644fe2237108472acbcde3123b186d2475dd5a23480321c78084746f9e
  The original CD Mcm2.exe uses SafeDisc copy protection, which Windows 10
  and 11 cannot run.
- A graphics card with Direct3D 11.
- Windows XP SP3 also works: there is no Direct3D 11 there, so the
  Direct3D 9 renderer is used, and saves go to
  Local Settings\Application Data\Motocross Madness 2.

Installing
----------
Copy d3drm.dll, mcm2fix.ini, MCM2HELP.html and the FONTS and SOUNDS folders
into the game folder, next to Mcm2.exe, and start Mcm2.exe. If a different Mcm2.exe
is found, the fix does nothing and says so in the log.

d3drm.dll is mcm2fix itself. The game loads a library of that name, which
Windows no longer ships, and Windows looks in the game folder first.

Settings
--------
Edit mcm2fix.ini (README.md has the full list too). Every setting is explained there, and a missing setting
uses its default. The file has three parts: preferences to change to taste;
advanced settings that keep the game working and playing correctly (physics
and timing, compatibility, sound), which are best left alone; and logging,
where only the main log is on.

Uninstalling
------------
Delete d3drm.dll, mcm2fix.ini, MCM2HELP.html, README.txt, README.md and the
FONTS, SOUNDS and licenses folders from the game folder.
Settings are kept in HKEY_CURRENT_USER\Software\mcm2fix\Motocross Madness 2,
and saves and the log in %LOCALAPPDATA%\Motocross Madness 2.

Problems
--------
Each run writes a log to %LOCALAPPDATA%\Motocross Madness 2\logs (on
Windows XP: Local Settings\Application Data\Motocross Madness 2\logs).
Please include the newest one in any report.

Source and licences
-------------------
Source: https://github.com/adelpha/mcm2fix

The video decoder is FFmpeg (https://ffmpeg.org), version n7.1, built with
only its Indeo 5 decoder. It is linked statically, under the GNU Lesser
General Public License 2.1 or later (see licenses\FFmpeg-LGPL-2.1.txt).
FFmpeg's source is the n7.1 release. The build script that produces this
DLL is tools/build-ffmpeg.sh in the mcm2fix source, so the DLL can be
rebuilt against a modified FFmpeg.

The HUD fonts in FONTS are Contrail One (c) 2011 Sorkin Type Co and DSEG7
Classic (c) 2017 keshikan, both under the SIL Open Font License 1.1 (see
licenses\ContrailOne-OFL.txt and licenses\DSEG-OFL.txt).

The announcer sounds in SOUNDS come from Pixabay (https://pixabay.com),
under the Pixabay Content License.

Motocross Madness 2 is (c) Microsoft / Rainbow Studios. This fix contains
no game files.
