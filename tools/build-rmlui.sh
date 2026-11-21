#!/bin/sh
# Builds static FreeType and RmlUi (the HTML/CSS-style menu library used by
# MenuMaX, see code/src/ui/) for 32-bit Windows with MinGW.
#
#   tools/build-rmlui.sh <work dir>
#
# The result is installed in <work dir>/install; pass that to CMake as
# -DMCM2MaX_RMLUI=<work dir>/install. Without it the DLL is built without
# the new menus. RmlUi is MIT; FreeType is under the FreeType License (FTL).
set -eu
WORK=$(mkdir -p "$1" && cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLCHAIN="$ROOT/code/toolchain-mingw32.cmake"
FREETYPE_VERSION=VER-2-13-3
RMLUI_VERSION=6.1
JOBS=$(nproc)

if [ ! -d "$WORK/freetype" ]; then
  git clone -q --depth 1 --branch "$FREETYPE_VERSION" https://github.com/freetype/freetype.git "$WORK/freetype"
fi
if [ ! -d "$WORK/rmlui" ]; then
  git clone -q --depth 1 --branch "$RMLUI_VERSION" https://github.com/mikke89/RmlUi.git "$WORK/rmlui"
fi

# FreeType: TrueType/OpenType only, none of the optional compression or
# shaping libraries (so nothing else needs to be shipped).
cmake -S "$WORK/freetype" -B "$WORK/build-freetype" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/install" -DBUILD_SHARED_LIBS=OFF \
  -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON \
  -DFT_DISABLE_BROTLI=ON -DCMAKE_C_FLAGS="-O2 -march=i686"
cmake --build "$WORK/build-freetype" -j"$JOBS"
cmake --install "$WORK/build-freetype"

cmake -S "$WORK/rmlui" -B "$WORK/build-rmlui" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/install" -DBUILD_SHARED_LIBS=OFF \
  -DRMLUI_FONT_ENGINE=freetype -DRMLUI_SAMPLES=OFF -DBUILD_TESTING=OFF -DRMLUI_PRECOMPILED_HEADERS=OFF \
  -DCMAKE_PREFIX_PATH="$WORK/install" -DFreetype_ROOT="$WORK/install" \
  -DCMAKE_CXX_FLAGS="-O2 -march=i686"
cmake --build "$WORK/build-rmlui" -j"$JOBS"
cmake --install "$WORK/build-rmlui"
echo "RmlUi installed in $WORK/install"
