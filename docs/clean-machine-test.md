# Clean-machine test

Run this before a release on a Windows 11 install that has never had the
game or any fix on it, for example a new VM or a new local user account on
a PC where nothing below exists. The aim is to prove that the zip alone is
enough.

## Before

Check that none of these exist. If any do, remove them or use another machine.

- `C:\Windows\SysWOW64\d3drm.dll`
- Any Indeo codec (`ir50_32.dll` in SysWOW64)
- `HKCU\Software\mcm2fix` and `HKCU\Software\Microsoft\Microsoft Games\Motocross Madness 2`
- `HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft Games\Motocross Madness 2`
- Compatibility flags for Mcm2.exe under
  `HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers`
  (and the same key under HKLM)
- `%LOCALAPPDATA%\Motocross Madness 2`

## Install

1. Put the game files in a folder the test user owns, for example
   `C:\Games\MCM2`. Use a copy of an installed game folder or the files from
   the CD, with the supported `Mcm2.exe` (see `package/README.txt`).
   Record which method was used.
2. Unzip the release into that folder: `d3drm.dll`, `mcm2fix.ini`,
   `MCM2HELP.html`, `README.txt`, `README.md`, `FONTS\`, `licenses\`.
3. Do not change any compatibility settings. Run as a standard
   (non-administrator) user.

## Run

Double-click `Mcm2.exe` and check each item:

- [ ] No UAC prompt and no compatibility assistant.
- [ ] No adapter picker, no hardware test screen, no "detected a change" box.
- [ ] The desktop resolution never changes, and the window is borderless at
      desktop size on the primary monitor.
- [ ] The intro video plays with sound and can be skipped.
- [ ] The main menu Single Player preview video plays after about 5 seconds.
- [ ] Single player race: menus, loading, race, pause, FINAL LAP, podium.
- [ ] Restart Race works and FINAL LAP shows again.
- [ ] Options: change the resolution to a widescreen mode. The view is wider,
      not stretched, and the speedometer stays round.
- [ ] Alt-Tab, Win+D and the second monitor (if any) behave.
- [ ] Credits play with music.
- [ ] Quit, start again: the resolution and controller choice are remembered,
      and the hardware test does not come back.

## After

- [ ] The newest `%LOCALAPPDATA%\Motocross Madness 2\logs\mcm2fix-*.log` exists. It starts with
      "mcm2fix loaded" and has no "not applied" or "skipped" lines.
- [ ] Nothing new in the game folder except what the zip added. Profiles and
      saves are under `%LOCALAPPDATA%\Motocross Madness 2`.
- [ ] Settings are under `HKCU\Software\mcm2fix\Motocross Madness 2`, and
      nothing was written under HKLM.
- [ ] Process Monitor (optional): filter on Mcm2.exe and Result
      "ACCESS DENIED". Nothing important should show up.

Keep the log with the test notes.
