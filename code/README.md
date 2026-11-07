# mcm2fix.dll

Loaded by `Mcm2.exe` in place of `d3drm.dll` (see `tools/mcm2patch.py`).
It provides `D3DRMVectorRotate`, the only function the game used from
d3drm, and patches the game's code in memory from `DllMain`, before the
game's entry point runs. It does nothing unless it finds the supported exe
(image base 0x400000, entry 0x13d6f5, image size 0x296000).

Current fixes (`src/fixes_step1.cpp`):
- flip-retry: restore and retry `Flip` on `DDERR_SURFACELOST` during the
  refresh-rate timing loop after the mode switch.
- one-core: `SetProcessAffinityMask(..., 1)` (stopgap for a thread race).
- no-admin: skip the startup administrator probe.

Diagnostics go to `OutputDebugString` with a `[mcm2fix]` prefix (visible in
DebugView or a debugger).

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
