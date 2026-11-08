Motocross Madness 2 fix (mcm2fix)
=================================

Makes Motocross Madness 2 run on Windows 10 and 11 by double-clicking
Mcm2.exe: no compatibility settings, no administrator rights, no DirectX
wrappers.

- Draws with Direct3D 11 in a borderless window at your desktop
  resolution. The display mode never changes and there is no adapter picker.
- Supports widescreen resolutions, with a wider field of view to match.
- Plays the intro, credits and menu videos itself, so the Indeo codec and
  DirectShow are not needed.
- Keeps settings in your user registry (HKCU) and saves in
  %LOCALAPPDATA%\Motocross Madness 2. Nothing is written to the game folder
  or HKLM.
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

Installing
----------
Copy d3drm.dll and mcm2fix.ini into the game folder, next to Mcm2.exe, and
start Mcm2.exe. If a different Mcm2.exe is found, the fix does nothing and
says so in the log.

d3drm.dll is mcm2fix itself. The game loads a library of that name, which
Windows no longer ships, and Windows looks in the game folder first.

Settings
--------
Edit mcm2fix.ini. Every setting is explained there, and a missing setting
uses its default.

Uninstalling
------------
Delete d3drm.dll and mcm2fix.ini from the game folder. Settings are kept
in HKEY_CURRENT_USER\Software\mcm2fix\Motocross Madness 2, and saves and
the log in %LOCALAPPDATA%\Motocross Madness 2.

Problems
--------
The log is %LOCALAPPDATA%\Motocross Madness 2\mcm2fix.log. Please include
it in any report.

Source and licences
-------------------
Source: https://github.com/adelpha/mcm2fix

The video decoder is FFmpeg (https://ffmpeg.org), version n7.1, built with
only its Indeo 5 decoder. It is linked statically, under the GNU Lesser
General Public License 2.1 or later (see licenses\FFmpeg-LGPL-2.1.txt).
FFmpeg's source is the n7.1 release. The build script that produces this
DLL is tools/build-ffmpeg.sh in the mcm2fix source, so the DLL can be
rebuilt against a modified FFmpeg.

Motocross Madness 2 is (c) Microsoft / Rainbow Studios. This fix contains
no game files.
