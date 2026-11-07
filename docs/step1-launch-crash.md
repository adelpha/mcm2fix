# Step 1: why the game quits on Windows 11

Tested on Windows 11 24H2, RTX 3070 Ti, two 1920x1080 monitors, no-CD
Mcm2.exe (1,577,005 bytes, SHA-256 345e4f64…6f9e).

The game never crashes. WinMain returns 1 (or calls exit(0)) after one of
these checks fails.

## 1. Admin probe (0x4a1f1d-0x4a2005)

On NT the game writes `IsAdmin=1` to
`HKLM\Software\Microsoft\Microsoft Games\Motocross Madness 2` and creates a
temp file in the current directory. If either fails it shows "You do not
have administrative privileges on this machine" and exits. It only passes
today because Windows registry virtualization silently redirects the HKLM
write to `HKCU\Software\Classes\VirtualStore`. Patched out by `no-admin`.

## 2. Refresh-rate flip loop (0x4ce680, called from device creation)

Right after `SetDisplayMode(640, 480, 16)` the game creates a flipping
primary surface and flips it ~12 times with `DDFLIP_WAIT` to time the
refresh rate. On Windows 10/11 the mode change is often still settling and
`IDirectDrawSurface7::Flip` returns `DDERR_SURFACELOST` (0x887601C2) at
0x4ce81d. The game treats any failure as fatal, device creation returns 0,
`GameInit` (0x4c43a0) returns 0, and WinMain quits silently.

It is timing dependent: it passed whenever a debugger slowed the code down.

Fix (`flip-retry`): the three Flip calls in that loop call a helper in the
.text slack (0x554a80) that calls `Restore()` and retries (10 ms sleep, up to
300 tries) on `DDERR_SURFACELOST`.

## 3. Indeo 5 movies

`UI\INTRO.AVI`, `UI\EVENTS.AVI` and `UI\CREDITS.AVI` are Indeo 5 (IV50) with
MS ADPCM audio. Windows 11 ships no Indeo decoder, so the game spins forever
on the first frame of the intro (the Microsoft logo).

## Other notes

- The renderer picker lists Software Renderer plus one entry per DirectDraw
  device: the primary display driver (GUID 0) and one per monitor.
- `d3drm.dll` is imported (only `D3DRMVectorRotate`) and is not part of
  Windows 11; this PC has an XP copy in SysWOW64.
- `UseVideoCardIdx` and `DriverInfo\...` profiling results are cached in
  HKLM (virtualized). See `runtime-writes.md`.

## Applying the patch

    python tools/mcm2patch.py Mcm2.exe Mcm2_fixed.exe

Keystone is optional; without it the stored bytes are used.
