#!/usr/bin/env python3
"""Stitch the game's sky tiles into one picture, and cut an edited picture back.

The sky is a box: a 488x488 top face and four 488x248 side faces, stored as
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

Neighbouring tiles overlap by 8 pixels (each tile repeats the last 8
rows/columns of the one before it), so a tile advances 120 pixels and a
4-tile face is 488 pixels across. The stitched picture (1928x728) has the top face above side A and the four
sides in a row underneath, in horizon order B A D C, so the top face's bottom
edge and the whole horizon ring are seamless in the picture. The top face's
left, top and right edges meet sides B, C and D, which are not next to it in
the picture: keep colours there consistent by hand.

  python sky_tiles.py stitch <textures_dump folder> terra12 sky.png
  python sky_tiles.py split  sky.png <output folder> terra12 <textures_dump folder>

split writes 48 PNGs named like the dumped tiles (it reads the dump folder
only for the names), ready for the textures folder. The picture can be any
whole multiple of 1928x728 (e.g. 3856x1456 gives 256x256 tiles).
The ring wraps round, so split copies its first 8 columns over its last 8.
"""
import glob
import os
import sys

from PIL import Image

TILE = 128
OVERLAP = 8  # neighbouring tiles share 8 rows/columns of pixels
STEP = TILE - OVERLAP
RING = ["B", "A", "D", "C"]  # left to right in the picture
TOP_COL = RING.index("A") * 4  # top face sits above side A
SIDE_BASE = {"A": 0, "B": 8, "C": 16, "D": 24}
WIDTH = 16 * STEP + OVERLAP  # the ring wraps: the last 8 columns repeat the first 8
HEIGHT = 6 * STEP + OVERLAP


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
    """Tile index -> (x, y) of its top-left pixel in the stitched picture (scale 1)."""
    pos = {}
    for i in range(16):
        pos[32 + i] = ((TOP_COL + i % 4) * STEP, (i // 4) * STEP)
    for slot, side in enumerate(RING):
        base = SIDE_BASE[side]
        for i in range(8):
            pos[base + i] = ((slot * 4 + i % 4) * STEP, (4 + i // 4) * STEP)
    return pos


def stitch(dump, prefix, out):
    names = tile_names(dump, prefix)
    pic = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    for i, (x, y) in layout().items():
        t = Image.open(os.path.join(dump, names[i] + ".png")).convert("RGBA")
        if t.size != (TILE, TILE):
            t = t.resize((TILE, TILE), Image.LANCZOS)
        pic.paste(t, (x, y))
    pic.save(out)
    print("wrote", out)


def split(src, outdir, prefix, dump):
    names = tile_names(dump, prefix)
    pic = Image.open(src).convert("RGBA")
    if pic.size[0] * HEIGHT != pic.size[1] * WIDTH or pic.size[0] % WIDTH:
        sys.exit("picture must be %dx%d or a whole multiple, got %dx%d" % (WIDTH, HEIGHT, pic.size[0], pic.size[1]))
    k = pic.size[0] // WIDTH
    # The ring wraps round: make its last columns repeat its first ones.
    ring_top = 4 * STEP * k
    pic.paste(pic.crop((0, ring_top, OVERLAP * k, pic.size[1])), (16 * STEP * k, ring_top))
    os.makedirs(outdir, exist_ok=True)
    for i, (x, y) in layout().items():
        pic.crop((x * k, y * k, (x + TILE) * k, (y + TILE) * k)).save(os.path.join(outdir, names[i] + ".png"))
    print("wrote 48 tiles of %dx%d to %s" % (TILE * k, TILE * k, outdir))


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "stitch":
        stitch(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) == 6 and sys.argv[1] == "split":
        split(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    else:
        sys.exit(__doc__)
