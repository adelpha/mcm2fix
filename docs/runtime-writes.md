# Runtime writes in Mcm2.exe (no-CD exe, 1,577,005 bytes)

Goal: a standard (non-admin) user can run the game from a read-only install
folder. Every runtime write must go to a per-user location.

Addresses are virtual addresses in Mcm2.exe (image base 0x400000).

## Registry

All registry access goes through one helper class around 0x4c6000-0x4c6e00 and
always uses `HKEY_LOCAL_MACHINE` (0x80000002) with the key
`Software\Microsoft\Microsoft Games\Motocross Madness 2`.

| Call | Address | Access |
|---|---|---|
| RegOpenKeyExA | 0x468b7e | KEY_READ (0x20019) |
| RegOpenKeyExA | 0x4c61d1, 0x4c62d1 | KEY_ALL_ACCESS (0xf003f), used to delete `DriverInfo` subkeys |
| RegOpenKeyExA | 0x4c6447, 0x4c6577, 0x4c66a7, 0x4c67df, 0x4c692f | KEY_READ |
| RegCreateKeyExA | 0x4c6a75, 0x4c6ba5, 0x4c6ce5 | KEY_ALL_ACCESS, followed by RegSetValueExA |
| RegDeleteKeyA | 0x4c620d | deletes the cached `DriverInfo` tree |

Values written include FIRSTRUN, UseLastVideoCard, UseVideoCardIdx and the
per-adapter cache `DriverInfo\<adapter>\{Modes, NumberOfModes, ...}`.

Plan: settings go to HKCU, falling back to HKLM for reads. The install path
comes from the exe's own folder, not from `HardDriveRootPath`.

## Files

Most file I/O goes through the CRT `fopen`. Write-mode call sites:

| Mode | Address | What |
|---|---|---|
| wb | 0x42525c | BMP writer (bmpfile.cpp), screenshots |
| wb | 0x4595d1 | `.esb` collision export |
| wb | 0x4afb35 | Net.cpp |
| wb | 0x4d88a7 | Pro circuit save (ProCircuit.cpp) |
| wb | 0x4ebe9f | to identify |
| wb | 0x51807d | to identify |
| wb | 0x524840 | `%s\%s` rider/profile path |
| wb | 0x528bde | `ui\profile\%s\%s.prf` |
| wb | 0x52a381 | replays (VCRfile.cpp: VCRtape.dat, VCRghost.dat) |
| w  | 0x459bbf | `.txt` dump |
| w  | 0x4a1f98 | "IsAdmin" probe: GetTempFileNameA (0x4a1f8c) in the game folder, then delete |
| w  | 0x4f4aef | `scnmgr.log` |
| a  | 0x511f53 | `C:\temp\TM_debug.txt` (debug log) |
| rb+ | 0x455374, 0x455d6d | `messbox1.dtm` / track list (dlgprocs.cpp) |
| rb+ | 0x4ebef4 | to identify |

Other writers: WritePrivateProfileStringA 0x47ca52 (audio_08.ini / audio_16.ini),
CreateDirectoryA 0x452ac6, CopyFileA 0x421acd / 0x452d75 / 0x4b956c,
DeleteFileA 0x44b9de / 0x4552ee / 0x455ce0 / 0x4a2001 / 0x4d9cf6,
RemoveDirectoryA 0x44b9ff.

Plan: write to `%LOCALAPPDATA%\Motocross Madness 2\`, and read user data from
there first, then from the install folder.
