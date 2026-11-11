# mcm2sky – sky box extract / stitch / split

`mcm2sky.exe` pulls every sky box out of the game's `RES\SKIES.RES`, stitches
each one into a single picture you can paint on, and cuts an edited picture
back into the 48 tiles the game uses, named so mcm2fix's texture replacement
picks them up (`textures\<file>_<offset>.png`). It reads the original game
files and never changes them.

## Use

1. Put `mcm2sky.exe` anywhere and double-click it. It looks for the game
   (a folder with `RES\SKIES.RES`) next to the tool, in a `mcm2` folder next
   to it, or one or two folders up. If it can't find it, drag the game folder
   onto `mcm2sky.exe`.
2. It writes `skies\` next to the tool:
   * `<sky>_sky.png` for each of the 14 skies (1928x728),
   * `tracks.txt` – which track uses which sky (e.g. Baja tracks use `terra12`),
   * `skies.txt` – the tile names, used by the split step. Keep it next to the
     pictures.
3. Edit a picture. Keep its size, or scale it up by a whole number
   (2x = 3856x1456 gives 256x256 tiles, 4x gives 512x512). Keep the start of the
   file name (`terra12_sky.png`, `terra12_sky_v2.png` are fine).
4. Drag the edited PNG(s) onto `mcm2sky.exe`. The tiles go to `textures\` next
   to the picture. Copy them into
   `%LOCALAPPDATA%\Motocross Madness 2\textures\` (needs `TextureReplace=1`,
   the default).

Command line:

```
mcm2sky extract [game folder] [-o folder]
mcm2sky split <picture.png>... [-o folder] [--seams average|none] [--game folder]
```

## The picture

```
                 +--------+
                 |  top   |
                 |  face  |
        +--------+--------+--------+--------+
        |   B    |   A    |   D    |   C    |   <- upper row
        |        |        |        |        |   <- horizon row
        +--------+--------+--------+--------+
```

* Each face is 4 tiles across; neighbouring tiles share 8 pixels (each tile
  repeats the last 8 rows/columns of the one before it), so a face is 488
  pixels, not 512, and the whole picture is 1928x728. Split cuts the tiles back
  out with the same 8-pixel overlap, so they always agree with each other.
* The horizon runs B → A → D → C and wraps: C's right edge meets B's left.
* The top face's bottom edge meets A's top (continuous in the picture). Its
  right edge meets D's top (read bottom to top), its top edge meets C's top
  (read right to left) and its left edge meets B's top (read top to bottom).
  Those are not next to each other in the picture, so split makes them match:
  every pixel the game shows twice (those edges and the horizon wrap) is set to
  the average of its copies. Use `--seams none` to turn that off. Paint across
  those edges gently and the average hides the join.
* The area beside the top face is not used.

## Format notes

See the comment at the top of `mcm2sky.cpp`: the archive encryption, the RS2
directory, the `.cub` texture records and the LZW coder, with the Mcm2.exe
addresses they were read from.

Build: `i686-w64-mingw32-g++ -O2 -std=c++17 -static -s -o mcm2sky.exe mcm2sky.cpp`
(or any C++17 compiler; it also builds on Linux). PNG reading/writing is
[stb_image](https://github.com/nothings/stb) (public domain), in `third_party\`.
