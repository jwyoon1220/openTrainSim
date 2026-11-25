# Cross-compiling the Windows build from Linux with MinGW-w64 (GCC: mingw-w64-gcc, or Clang:
# llvm-mingw). Put the toolchain's bin directory on PATH (or set MINGW_PREFIX), then:
#   cmake --preset windows-cross-mingw   (or: cmake -B build-mingw -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake)
# The Vulkan loader is loaded at run time (volk); the headers are fetched.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE x86_64-w64-mingw32)
if (DEFINED ENV{MINGW_PREFIX})
    set(MINGW_PREFIX $ENV{MINGW_PREFIX})
endif ()
if (MINGW_PREFIX)
    set(_bin ${MINGW_PREFIX}/bin/)
endif ()
# llvm-mingw names its compilers <triple>-clang; mingw-w64-gcc <triple>-gcc.
find_program(_mingw_cc NAMES ${MINGW_TRIPLE}-clang ${MINGW_TRIPLE}-gcc HINTS ${_bin} NO_CACHE)
if (_mingw_cc MATCHES "clang$")
    set(CMAKE_C_COMPILER ${_bin}${MINGW_TRIPLE}-clang)
    set(CMAKE_CXX_COMPILER ${_bin}${MINGW_TRIPLE}-clang++)
else ()
    set(CMAKE_C_COMPILER ${_bin}${MINGW_TRIPLE}-gcc)
    set(CMAKE_CXX_COMPILER ${_bin}${MINGW_TRIPLE}-g++)
endif ()
set(CMAKE_RC_COMPILER ${_bin}${MINGW_TRIPLE}-windres)

# Libraries and headers come from the toolchain sysroot (<bin>/../<triple>: /usr/x86_64-w64-mingw32
# for distribution packages, the same layout in llvm-mingw); programs (glslc) from the host.
get_filename_component(_mingw_root "${_mingw_cc}" DIRECTORY)
get_filename_component(_mingw_root "${_mingw_root}/../${MINGW_TRIPLE}" ABSOLUTE)
set(CMAKE_FIND_ROOT_PATH ${_mingw_root})
# Keep pkg-config away from the host's (Linux) libraries.
set(ENV{PKG_CONFIG_LIBDIR} ${_mingw_root}/lib/pkgconfig)
set(ENV{PKG_CONFIG_PATH} "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
