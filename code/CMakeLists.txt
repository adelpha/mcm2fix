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
  mcm2fix.def)
set_target_properties(mcm2fix PROPERTIES PREFIX "" OUTPUT_NAME "mcm2fix")

if(MSVC)
  # Static CRT so the DLL needs no Visual C++ redistributable.
  set_property(TARGET mcm2fix PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(mcm2fix PRIVATE /W4 /permissive-)
else()
  target_compile_options(mcm2fix PRIVATE -Wall -Wextra -O2)
  target_link_options(mcm2fix PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()
