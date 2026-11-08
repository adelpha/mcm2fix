cmake_minimum_required(VERSION 3.16)
project(mcm2fix CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "mcm2fix must be built as 32-bit (x86 / Win32)")
endif()

add_library(mcm2fix SHARED
  src/dllmain.cpp
  src/patch.cpp
  src/d3drm_compat.cpp
  src/fixes_step1.cpp
  src/fixes_startup.cpp
  src/fixes_registry.cpp
  src/fixes_files.cpp
  src/fixes_widescreen.cpp
  src/fixes_detail.cpp
  src/fixes_arrow.cpp
  src/fixes_restart.cpp
  src/diagnostics.cpp
  src/regtrace.cpp
  src/config.cpp
  src/dd/backend.cpp
  src/dd/surface.cpp
  src/dd/directdraw.cpp
  src/dd/device.cpp
  src/dd/hook.cpp
  src/dd/msaa.cpp
  src/dd/texreplace.cpp
  src/video/avi.cpp
  src/video/adpcm.cpp
  src/video/audio_out.cpp
  src/video/player.cpp
  mcm2fix.def)
# Indeo 5 video decoding needs a static FFmpeg (tools/build-ffmpeg.sh). Without
# it the game's own DirectShow video path is used.
set(MCM2FIX_FFMPEG "" CACHE PATH "FFmpeg install prefix from tools/build-ffmpeg.sh")
if(MCM2FIX_FFMPEG)
  target_sources(mcm2fix PRIVATE src/video/indeo5_ffmpeg.cpp)
  target_include_directories(mcm2fix PRIVATE "${MCM2FIX_FFMPEG}/include")
  target_link_libraries(mcm2fix PRIVATE "${MCM2FIX_FFMPEG}/lib/libavcodec.a"
                                        "${MCM2FIX_FFMPEG}/lib/libavutil.a" bcrypt)
else()
  target_sources(mcm2fix PRIVATE src/video/indeo5_stub.cpp)
endif()

set_target_properties(mcm2fix PROPERTIES PREFIX "" OUTPUT_NAME "mcm2fix")

target_link_libraries(mcm2fix PRIVATE d3d11 dxgi dxguid gdi32 user32 ole32 windowscodecs winmm)
target_compile_definitions(mcm2fix PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
  # Static CRT so the DLL needs no Visual C++ redistributable.
  set_property(TARGET mcm2fix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(mcm2fix PRIVATE /W4 /permissive-)
else()
  target_compile_options(mcm2fix PRIVATE -Wall -Wextra -O2)
  target_link_options(mcm2fix PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()
