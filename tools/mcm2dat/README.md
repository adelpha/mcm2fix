# mcm2dat

Unpacks Motocross Madness 2's menu archives (`UI\*.DAT`) so you can look at
the original menu graphics and page layouts. The game's files are only read.

    mcm2dat <file.DAT or folder> [more...] [-o <output folder>]

Or drag a `.DAT` file, or the whole `UI` folder, onto `mcm2dat.exe`. Output
goes to `mcm2dat-out\<DAT name>\` next to `mcm2dat.exe` (or the `-o` folder),
keeping each file's path inside the archive, e.g.
`mcm2dat-out\OPTGRAPH\Options\Graphics\GPlate01.tga`.

- Every file is copied out unchanged (`.tga` sprites, the page's `.dtm`
  layout, any `.wav`).
- Each `.tga` is also saved as a `.png` beside it. The sprites are 16-bit
  (5:5:5) with no alpha channel.
- `_contents.txt` in each folder lists the archive's files with their offset
  and size.
- Files that are not RS2 archives are skipped.

## The RS2 format

Little-endian, no compression:

    "RS2\0"                      4 bytes
    u32 count
    count times:
      u32 name length            includes the terminating NUL
      name                       e.g. "Options\Graphics\GPlate01.tga"
      u32 offset                 from the start of the file
      u32 size

## The .dtm layout

INI-like text exported from a Visual Basic form designer. `[Set_Control]`
starts the controls: `[Set_Information]` describes the page, then one
`[Name]` section per control with `Type=` (`BUTTON`, `STATIC`, `STATICTEXT`,
`MULTISTATE` (checkbox), `RADIOBUTTON`, `DROPDOWNLIST`, `HSCROLLBAR`,
`VSCROLLBAR`, `LISTBOX`, `EDITBOX`...), `Xywh=` (position and size in 640x480
pixels, relative to `Anchor=` if given), `Text=`, `ToolTipText=` and the
sprite names it uses (`AnimNorm`, `AnimFocus`, `AnimPush`, `State0`...).
`[Set_Anim]` starts the sprites: `[Name]` with `Files1=` the `.tga` path.
`[Set_Sound]` likewise for sounds. Section names are the developers' own and
can be out of date (the Advanced Options "Graphics Quality" box is called
`UnusedGraphicsSlideBox`). Sprites a page uses but does not list
(`CheckBoxOn`, `DownArrowUp`...) come from the shared `GLOBAL.DAT`.

Build: `i686-w64-mingw32-g++ -O2 -std=c++17 -static -s -o mcm2dat.exe tools/mcm2dat/mcm2dat.cpp`
(uses the stb headers in `tools/mcm2sky/third_party`).
