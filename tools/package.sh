#!/bin/sh
# Makes the release zip: d3drm.dll (mcm2fix, drop-in next to Mcm2.exe),
# mcm2fix.ini with every setting at its default, README.txt, README.md, licences, the
# HUD fonts (FONTS\, SIL Open Font License) and the race announcer sounds
# (SOUNDS\, played by Announcer=1).
#
#   tools/package.sh <mcm2fix.dll> <output zip> [MCM2HELP.HLP]
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

cp "$DLL" "$STAGE/d3drm.dll"
if command -v i686-w64-mingw32-strip >/dev/null 2>&1; then
  i686-w64-mingw32-strip "$STAGE/d3drm.dll"
fi
# The ini block documented in code/README.md, with Windows line endings.
awk '/^```ini$/{on=1; next} /^```$/{if(on) exit} on' "$ROOT/code/README.md" |
  sed 's/$/\r/' > "$STAGE/mcm2fix.ini"
grep -q '^\[mcm2fix\]' "$STAGE/mcm2fix.ini"
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
# folder of its own; mcm2fix reads them from SOUNDS\ next to Mcm2.exe.
mkdir "$STAGE/SOUNDS"
cp "$ROOT"/package/SOUNDS/*.wav "$STAGE/SOUNDS/"

if [ -n "$HLP" ]; then
  python3 "$ROOT/tools/hlp2html.py" "$HLP" "$STAGE/MCM2HELP.html"
else
  cp "$ROOT/package/MCM2HELP.html" "$STAGE/MCM2HELP.html"
fi
FILES="d3drm.dll mcm2fix.ini README.txt README.md licenses MCM2HELP.html FONTS SOUNDS"

rm -f "$OUT"
OUT_ABS=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
(cd "$STAGE" && zip -q -r "$OUT_ABS" $FILES)
echo "wrote $OUT"
