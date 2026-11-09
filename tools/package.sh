#!/bin/sh
# Makes the release zip: d3drm.dll (mcm2fix, drop-in next to Mcm2.exe),
# mcm2fix.ini with every setting at its default, README.txt and licences.
#
#   tools/package.sh <mcm2fix.dll> <output zip> [MCM2HELP.HLP]
#
# With the game's MCM2HELP.HLP, the zip also gets MCM2HELP.html (made by
# tools/hlp2html.py), which the in-game Help buttons open.
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
mkdir "$STAGE/licenses"
sed 's/$/\r/' "$ROOT/package/licenses/FFmpeg-LGPL-2.1.txt" > "$STAGE/licenses/FFmpeg-LGPL-2.1.txt"

FILES="d3drm.dll mcm2fix.ini README.txt licenses"
if [ -n "$HLP" ]; then
  python3 "$ROOT/tools/hlp2html.py" "$HLP" "$STAGE/MCM2HELP.html"
  FILES="$FILES MCM2HELP.html"
fi

rm -f "$OUT"
OUT_ABS=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
(cd "$STAGE" && zip -q -r "$OUT_ABS" $FILES)
echo "wrote $OUT"
