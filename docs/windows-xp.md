# Windows XP support

Goal: the same `d3drm.dll` (mcm2fix) loads and runs on Windows XP SP3 as well
as Windows 7 to 11. Only the MinGW build targets XP; current MSVC toolsets and
their static CRT need Windows 7.

## What stopped mcm2fix loading on XP (audit 2026-11-09)

A DLL whose import table names a function or DLL the system lacks does not
load at all, and the game then fails to start. The release2 DLL imported:

| Import | From | Fix |
| --- | --- | --- |
| `d3d11.dll`, `dxgi.dll` | renderer | loaded with `LoadLibrary` at run time (`dd/backend.cpp`, `dd/hook.cpp`) |
| `bcrypt.dll` | FFmpeg `av_get_random_seed` | `HAVE_BCRYPT` switched off in `tools/build-ffmpeg.sh` |
| SRW locks, `InitOnce*` | FFmpeg Win32 threads | FFmpeg built without threads (one small video at a time) |
| condition variables, `GetThreadId` | GCC 13 "win32" thread model (libgcc gthr-win32) | build with the "posix" thread-model compiler (`toolchain-mingw32.cmake`) |
| `GetTickCount64` | winpthreads (posix model) | import slot supplied by `src/xp_compat.cpp` (real function or GetTickCount fallback) |
| `RegCopyTreeA`, `RegDeleteTreeA` | registry import / DriverInfo reset | `xp::CopyRegTree` / `xp::DeleteRegTree`: real call if present, else `SHCopyKeyA` / `SHDeleteKeyA` |
| `CreateWaitableTimerExW` | FrameCap timer | `xp::CreateFrameTimer`: falls back to `CreateWaitableTimerW` |
| `strncat_s` | msvcrt (Vista's only) | `xp::Append` |

`tools/check-xp-imports.py <dll>` lists anything left that XP lacks; CI runs
it on every MinGW build.

## Behaviour differences on XP

- **Per-user folder**: XP has no `%LOCALAPPDATA%`. Saves, logs and textures go
  to `SHGetFolderPath(CSIDL_LOCAL_APPDATA)`, i.e.
  `C:\Documents and Settings\<user>\Local Settings\Application Data\Motocross Madness 2`.
- **Renderer**: Direct3D 11 does not exist on XP, so the renderer falls back
  to the fixed-function Direct3D 9 renderer (`dd/backend9.cpp`, d3d9.dll
  loaded at run time; ini `Renderer=D3D9` forces it). Only with no Direct3D 9
  at all does mcm2fix leave the game on Windows' own DirectDraw.
- **Texture replacement/dump** use WIC, which XP SP3 has only with .NET 3.0+
  or the WIC redistributable; without it they log a failure and do nothing.
- Registry: HKCU works the same. XP has no UAC virtualisation, so the HKLM
  import on first run finds only what an XP install wrote there.

- **VSync**: without a desktop compositor, a windowed Direct3D 9 present is a
  plain blit and XP drivers often ignore its interval. With VSync=1 the D3D9
  renderer waits for the vertical blank itself (log line `vsync:`).
  `Fullscreen=Exclusive` avoids the issue altogether: the driver flips.

## The log

Each run starts with lines like

    system: Windows 5.1 build 2600 Service Pack 3 (SP 3.0)
    system: local app data from SHGetFolderPath(CSIDL_LOCAL_APPDATA): C:\Documents and Settings\...
    system: GetTickCount64 no (XP fallback), RegCopyTreeA no (XP fallback), ...
    d3d11 backend: d3d11.dll missing, dxgi.dll missing (entry points missing)

which say which fallbacks were taken.
