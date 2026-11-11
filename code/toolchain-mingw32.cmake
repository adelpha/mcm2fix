set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
# Prefer the "posix" thread-model compilers (Debian/Ubuntu ship both). GCC 13+'s
# "win32" thread model builds std::mutex and thread-safe statics on Vista-only
# condition variables, which would stop the DLL loading on Windows XP;
# winpthreads only needs GetTickCount64, which src/xp_compat.cpp supplies.
find_program(MCM2FIX_GXX_POSIX i686-w64-mingw32-g++-posix)
if(MCM2FIX_GXX_POSIX)
  set(CMAKE_C_COMPILER i686-w64-mingw32-gcc-posix)
  set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++-posix)
else()
  set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
  set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
endif()
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
