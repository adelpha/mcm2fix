cmake_minimum_required(VERSION 3.16)
project(MCM2MaX CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "MCM2MaX must be built as 32-bit (x86 / Win32)")
endif()

add_library(MCM2MaX SHARED
  src/dllmain.cpp
  src/patch.cpp
  src/d3drm_compat.cpp
  src/fixes_step1.cpp
  src/fixes_startup.cpp
  src/fixes_registry.cpp
  src/fixes_files.cpp
  src/fixes_widescreen.cpp
  src/fixes_resolution.cpp
  src/display.cpp
  src/fixes_detail.cpp
  src/fixes_arrow.cpp
  src/fixes_restart.cpp
  src/fixes_help.cpp
  src/fixes_textures.cpp
  src/fixes_grid.cpp
  src/power.cpp
  src/fixes_perf.cpp
  src/fixes_sound.cpp
  src/fixes_reverb.cpp
  src/fixes_sort.cpp
  src/fixes_walk.cpp
  src/fartrees.cpp
  src/gputerrain.cpp
  src/fixes_terrain.cpp
  src/fixes_ai.cpp
  src/diagnostics.cpp
  src/crash.cpp
  src/regtrace.cpp
  src/config.cpp
  src/framerate.cpp
  src/freecam.cpp
  src/input_timing.cpp
  src/xinput_pad.cpp
  src/prof.cpp
  src/simd.cpp
  src/simd_plain.cpp
  src/simd_sse2.cpp
  src/terrain_fast.cpp
  src/smooth.cpp
  src/smooth_parts.cpp
  src/xp_compat.cpp
  src/dd/backend.cpp
  src/dd/backend9.cpp
  src/dd/device9.cpp
  src/dd/surface.cpp
  src/dd/directdraw.cpp
  src/dd/device.cpp
  src/dd/gdihook.cpp
  src/dd/gputext.cpp
  src/dd/drawlog.cpp
  src/dd/motionblur.cpp
  src/dd/hudtext.cpp
  src/dd/hires3d.cpp
  src/dd/racetitle.cpp
  src/dd/oneshot.cpp
  src/dd/hook.cpp
  src/dd/msaa.cpp
  src/dd/menubg.cpp
  src/dd/texreplace.cpp
  src/dd/texdecode.cpp
  src/dd/probe.cpp
  src/dd/reflect.cpp
  src/dd/gpushadow.cpp
  src/video/avi.cpp
  src/video/adpcm.cpp
  src/video/audio_out.cpp
  src/video/player.cpp
  src/music.cpp
  src/menus.cpp
  src/menunav.cpp
  src/menugame.cpp
  src/music_decoders.cpp
  MCM2MaX.def)
# Indeo 5 video decoding needs a static FFmpeg (tools/build-ffmpeg.sh). Without
# it the game's videos are skipped.
set(MCM2MaX_FFMPEG "" CACHE PATH "FFmpeg install prefix from tools/build-ffmpeg.sh")
if(MCM2MaX_FFMPEG)
  target_sources(MCM2MaX PRIVATE src/video/indeo5_ffmpeg.cpp)
  target_include_directories(MCM2MaX PRIVATE "${MCM2MaX_FFMPEG}/include")
  target_link_libraries(MCM2MaX PRIVATE "${MCM2MaX_FFMPEG}/lib/libavcodec.a"
                                        "${MCM2MaX_FFMPEG}/lib/libavutil.a")
else()
  target_sources(MCM2MaX PRIVATE src/video/indeo5_stub.cpp)
endif()

# Controllers (ControllerHub): a static input-only SDL3 (tools/build-sdl3.sh).
# Without it ControllerHub is unavailable and XInput pads use xinput_pad.cpp alone.
set(MCM2MaX_SDL3 "" CACHE PATH "SDL3 install prefix from tools/build-sdl3.sh")
if(MCM2MaX_SDL3)
  target_sources(MCM2MaX PRIVATE src/pad_hub.cpp)
  target_compile_definitions(MCM2MaX PRIVATE MCM2MAX_SDL3=1)
  target_include_directories(MCM2MaX PRIVATE "${MCM2MaX_SDL3}/include")
  target_link_libraries(MCM2MaX PRIVATE "${MCM2MaX_SDL3}/lib/libSDL3.a" imm32 oleaut32 version uuid advapi32 setupapi)
else()
  target_sources(MCM2MaX PRIVATE src/pad_hub_stub.cpp)
endif()

# MenuMaX screens need static RmlUi + FreeType (tools/build-rmlui.sh). Without
# them the DLL has stubs and the new screens are off.
set(MCM2MaX_RMLUI "" CACHE PATH "RmlUi + FreeType install prefix from tools/build-rmlui.sh")
if(MCM2MaX_RMLUI)
  target_sources(MCM2MaX PRIVATE src/ui/ui.cpp src/ui/gameart.cpp src/ui/render.cpp src/ui/render_d3d11.cpp src/ui/render_d3d9.cpp)
  target_include_directories(MCM2MaX PRIVATE "${MCM2MaX_RMLUI}/include")
  target_compile_definitions(MCM2MaX PRIVATE RMLUI_STATIC_LIB)
  target_link_libraries(MCM2MaX PRIVATE "${MCM2MaX_RMLUI}/lib/librmlui.a" "${MCM2MaX_RMLUI}/lib/libfreetype.a")
else()
  target_sources(MCM2MaX PRIVATE src/ui/ui_stub.cpp)
endif()

set_target_properties(MCM2MaX PROPERTIES PREFIX "" OUTPUT_NAME "MCM2MaX")

# d3d11.dll, dxgi.dll and d3d9.dll are loaded at run time (dd/backend*.cpp),
# so the DLL loads where they are missing and the renderer can fall back.
target_link_libraries(MCM2MaX PRIVATE dxguid gdi32 user32 ole32 winmm shell32 shlwapi)
target_compile_definitions(MCM2MaX PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# SSE2 kernels (simd_sse2.cpp) are only called after a CPU check; the rest
# of the DLL stays plain i686 so it still runs without SSE2. -mstackrealign:
# 32-bit Windows only keeps the stack 4-byte aligned.
if(NOT MSVC)
  set_source_files_properties(src/simd_sse2.cpp PROPERTIES COMPILE_OPTIONS "-msse2;-mstackrealign")
  # FastTerrain repeats the game's x87 maths exactly in SSE2 doubles: no
  # contraction into fused multiply-adds, no reordering.
  set_source_files_properties(src/terrain_fast.cpp PROPERTIES COMPILE_OPTIONS
    "-msse2;-mfpmath=sse;-mstackrealign;-ffp-contract=off;-fno-strict-aliasing")
  # Wuffs and stb_image_write (third_party): their own code, their own
  # warnings. Wuffs picks its SSE4.2/AVX2 paths at run time by CPU check.
  set_source_files_properties(src/dd/texdecode.cpp PROPERTIES COMPILE_OPTIONS "-w;-mstackrealign")
endif()

if(MSVC)
  # Static CRT so the DLL needs no Visual C++ redistributable.
  set_property(TARGET MCM2MaX PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(MCM2MaX PRIVATE /W4 /permissive-)
else()
  target_compile_options(MCM2MaX PRIVATE -Wall -Wextra -O2)
  target_link_options(MCM2MaX PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()

# Renderer smoke test (no game needed; runs on Windows or under Wine):
#   cmake -DMCM2MaX_TESTS=ON ...; dd_smoke.exe d3d9|d3d11
option(MCM2MaX_TESTS "Build tests/dd_smoke.exe" OFF)
if(MCM2MaX_TESTS)
  get_target_property(MCM2MAX_SOURCES MCM2MaX SOURCES)
  list(FILTER MCM2MAX_SOURCES EXCLUDE REGEX "dllmain\\.cpp|\\.def$")
  add_executable(dd_smoke tests/dd_smoke.cpp ${MCM2MAX_SOURCES})
  target_include_directories(dd_smoke PRIVATE src)
  if(MCM2MaX_FFMPEG)
    target_include_directories(dd_smoke PRIVATE "${MCM2MaX_FFMPEG}/include")
    target_link_libraries(dd_smoke PRIVATE "${MCM2MaX_FFMPEG}/lib/libavcodec.a"
                                           "${MCM2MaX_FFMPEG}/lib/libavutil.a")
  endif()
  if(MCM2MaX_SDL3)
    target_compile_definitions(dd_smoke PRIVATE MCM2MAX_SDL3=1)
    target_include_directories(dd_smoke PRIVATE "${MCM2MaX_SDL3}/include")
    target_link_libraries(dd_smoke PRIVATE "${MCM2MaX_SDL3}/lib/libSDL3.a" imm32 oleaut32 version uuid advapi32 setupapi)
  endif()
  target_link_libraries(dd_smoke PRIVATE dxguid gdi32 user32 ole32 winmm shell32 shlwapi)
  target_compile_definitions(dd_smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(NOT MSVC)
    # Loaded away from Mcm2.exe's addresses, which the test fills with zeros
    # (no game): the renderer reads a few game globals there.
    target_link_options(dd_smoke PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
                        -Wl,--image-base,0x10000000)
  endif()
endif()
