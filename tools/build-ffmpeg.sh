#!/bin/sh
# Builds a tiny static FFmpeg (libavcodec + libavutil, Indeo 5 decoder only)
# for 32-bit Windows with MinGW, for mcm2fix's video player.
#
#   tools/build-ffmpeg.sh <work dir>
#
# The result is installed in <work dir>/install; pass that to CMake as
# -DMCM2FIX_FFMPEG=<work dir>/install. FFmpeg is LGPL 2.1 or later.
set -eu
WORK=$(mkdir -p "$1" && cd "$1" && pwd)
VERSION=n7.1
if [ ! -d "$WORK/ffmpeg" ]; then
  git clone -q --depth 1 --branch "$VERSION" https://github.com/FFmpeg/FFmpeg.git "$WORK/ffmpeg"
fi
cd "$WORK/ffmpeg"
./configure --prefix="$WORK/install" \
  --target-os=mingw32 --arch=x86 --cross-prefix=i686-w64-mingw32- --enable-cross-compile \
  --disable-everything --enable-decoder=indeo5 \
  --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-avdevice --disable-avformat --disable-swscale --disable-swresample \
  --disable-avfilter --enable-static --disable-shared --disable-debug --enable-small \
  --disable-x86asm
make -j"$(nproc)"
make install
