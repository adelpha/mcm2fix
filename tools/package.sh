#!/bin/sh
# Makes the release zip: d3drm.dll (MCM2MaX, drop-in next to Mcm2.exe),
# mcm2max.ini with every setting at its default, README.txt, README.md, licences, the
# HUD fonts (FONTS\, SIL Open Font License) and the race announcer sounds
# (SOUNDS\, played by Announcer=1) and the MenuFlag main menu picture (UI\).
#
#   tools/package.sh <MCM2MaX.dll> <output zip> [MCM2HELP.HLP]
#
# The zip includes package/MCM2HELP.html (the game's help file converted by
# tools/hlp2html.py), which the in-game Help buttons open. Pass the game's
# MCM2HELP.HLP to convert it afresh instead.
set -eu
DLL=$1
OUT=$2
HLP=${3:-}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# Refuse a DLL built without FFmpeg (-DMCM2MaX_FFMPEG missing or misspelled):
# the game crashes at the first menu video without the Indeo 5 decoder.
if ! grep -aq "FfmpegIndeo5" "$DLL"; then
  echo "package.sh: $DLL has no FFmpeg Indeo 5 decoder (configure with -DMCM2MaX_FFMPEG=<build-ffmpeg.sh dir>/install); the game would crash at the menu video" >&2
  exit 1
fi
cp "$DLL" "$STAGE/d3drm.dll"
if command -v i686-w64-mingw32-strip >/dev/null 2>&1; then
  i686-w64-mingw32-strip "$STAGE/d3drm.dll"
fi
# The ini block documented in code/README.md, with Windows line endings.
awk '/^```ini$/{on=1; next} /^```$/{if(on) exit} on' "$ROOT/code/README.md" |
  sed 's/$/\r/' > "$STAGE/mcm2max.ini"
grep -q '^\[MCM2MaX\]' "$STAGE/mcm2max.ini"
sed 's/$/\r/' "$ROOT/package/README.txt" > "$STAGE/README.txt"
# The full project README (every fix, feature and setting).
sed 's/$/\r/' "$ROOT/README.md" > "$STAGE/README.md"
mkdir "$STAGE/licenses"
for f in "$ROOT"/package/licenses/*.txt; do
  sed 's/$/\r/' "$f" > "$STAGE/licenses/$(basename "$f")"
done
# HudFont / HudDigitFont defaults (see code/README.md).
mkdir "$STAGE/FONTS"
cp "$ROOT"/package/FONTS/*.ttf "$STAGE/FONTS/"
# Announcer sounds (Announcer in code/README.md). The game has no SOUNDS\
# folder of its own; MCM2MaX reads them from SOUNDS\ next to Mcm2.exe.
mkdir "$STAGE/SOUNDS"
cp "$ROOT"/package/SOUNDS/*.wav "$STAGE/SOUNDS/"

# The MenuFlag main menu background (MenuBackground in code/README.md): the
# game's own menu art with the backdrop cut out, read from the game folder.
mkdir -p "$STAGE/UI/MAINMENU/01_MAIN"
cp "$ROOT/package/UI/MAINMENU/01_MAIN/BackGround_01.png" "$STAGE/UI/MAINMENU/01_MAIN/"
# MenuMaX screens (RmlUi .rml/.rcss text files, pictures, fonts).
cp -r "$ROOT/package/UI/MaX" "$STAGE/UI/"
sed -i 's/$/\r/' "$STAGE/UI/MaX/README.txt"

if [ -n "$HLP" ]; then
  python3 "$ROOT/tools/hlp2html.py" "$HLP" "$STAGE/MCM2HELP.html"
else
  cp "$ROOT/package/MCM2HELP.html" "$STAGE/MCM2HELP.html"
fi
FILES="d3drm.dll mcm2max.ini README.txt README.md licenses MCM2HELP.html FONTS SOUNDS UI"

rm -f "$OUT"
OUT_ABS=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
(cd "$STAGE" && zip -q -r "$OUT_ABS" $FILES)
echo "wrote $OUT"
