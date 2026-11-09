#!/usr/bin/env python3
"""Stitch the game's sky tiles into one picture, and cut an edited picture back.

The sky is a box: a 512x512 top face and four 512x256 side faces, stored as
48 tiles of 128x128 in one terrain file (e.g. terra12 for Baja). TextureDump
writes them as <file>_<offset>.png; sorted by offset they are:

  tiles  0-7   side A (row of 4 upper tiles, then row of 4 horizon tiles)
  tiles  8-15  side B
  tiles 16-23  side C
  tiles 24-31  side D
  tiles 32-47  top face, 4 rows of 4

Going round the horizon the sides join A -> D -> C -> B -> A. The top face's
bottom edge joins side A's top edge; its right edge (read bottom to top) joins
D, its top edge (read right to left) joins C and its left edge joins B.

The stitched picture (2048x768) has the top face above side A and the four
sides in a row underneath, in horizon order B A D C, so the top face's bottom
edge and the whole horizon ring are seamless in the picture. The top face's
left, top and right edges meet sides B, C and D, which are not next to it in
the picture: keep colours there consistent by hand.

  python sky_tiles.py stitch <textures_dump folder> terra12 sky.png
  python sky_tiles.py split  sky.png <output folder> terra12 <textures_dump folder>

split writes 48 PNGs named like the dumped tiles (it reads the dump folder
only for the names), ready for the textures folder. The picture can be any
multiple of 2048x768 (e.g. 4096x1536 gives 256x256 tiles).
"""
import glob
import os
import sys

from PIL import Image

TILE = 128
# Placement of the faces in the stitched picture, in tile units: (column, row).
RING = ["B", "A", "D", "C"]  # left to right in the picture
TOP_COL = RING.index("A") * 4  # top face sits above side A
SIDE_BASE = {"A": 0, "B": 8, "C": 16, "D": 24}


def tile_names(dump, prefix):
    files = glob.glob(os.path.join(dump, prefix + "_*.png"))
    keyed = []
    for f in files:
        stem = os.path.splitext(os.path.basename(f))[0]
        try:
            keyed.append((int(stem[len(prefix) + 1:], 16), stem))
        except ValueError:
            pass
    keyed.sort()
    if len(keyed) != 48:
        sys.exit("expected 48 %s_* tiles in %s, found %d" % (prefix, dump, len(keyed)))
    return [stem for _, stem in keyed]


def layout():
    """Tile index -> (column, row) in the stitched picture."""
    pos = {}
    for i in range(16):
        pos[32 + i] = (TOP_COL + i % 4, i // 4)
    for slot, side in enumerate(RING):
        base = SIDE_BASE[side]
        for i in range(8):
            pos[base + i] = (slot * 4 + i % 4, 4 + i // 4)
    return pos


def stitch(dump, prefix, out):
    names = tile_names(dump, prefix)
    pic = Image.new("RGBA", (16 * TILE, 6 * TILE), (0, 0, 0, 0))
    for i, (c, r) in layout().items():
        t = Image.open(os.path.join(dump, names[i] + ".png")).convert("RGBA")
        if t.size != (TILE, TILE):
            t = t.resize((TILE, TILE), Image.LANCZOS)
        pic.paste(t, (c * TILE, r * TILE))
    pic.save(out)
    print("wrote", out)


def split(src, outdir, prefix, dump):
    names = tile_names(dump, prefix)
    pic = Image.open(src).convert("RGBA")
    if pic.size[0] * 6 != pic.size[1] * 16 or pic.size[0] % 16:
        sys.exit("picture must be 16:6 (2048x768 or a multiple), got %dx%d" % pic.size)
    t = pic.size[0] // 16
    os.makedirs(outdir, exist_ok=True)
    for i, (c, r) in layout().items():
        pic.crop((c * t, r * t, (c + 1) * t, (r + 1) * t)).save(os.path.join(outdir, names[i] + ".png"))
    print("wrote 48 tiles of %dx%d to %s" % (t, t, outdir))


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "stitch":
        stitch(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) == 6 and sys.argv[1] == "split":
        split(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    else:
        sys.exit(__doc__)
