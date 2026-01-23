# =============================================================================
# CMake Toolchain File for Cross-Compiling to Windows (MinGW-w64)
# =============================================================================
#
# This toolchain file enables cross-compilation from Linux to Windows using
# the MinGW-w64 toolchain.
#
# Usage:
#   cmake -B build-windows \
#         -DCMAKE_TOOLCHAIN_FILE=apps/gnostr-signer/packaging/windows/toolchain-mingw64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt install mingw-w64 mingw-w64-tools
#
# Prerequisites (Fedora):
#   sudo dnf install mingw64-gcc mingw64-gcc-c++ mingw64-gtk4 mingw64-libadwaita
#
# Note: Cross-compiling GTK4 applications is complex. Native Windows builds
# using MSYS2 are recommended for production.
#
# =============================================================================

# Target system
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Cross-compiler settings
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# Find the cross-compiler
find_program(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Use POSIX thread model (required for C++11 threads)
# If you have both win32 and posix variants, prefer posix:
# find_program(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc-posix)

# Target environment paths
# Adjust these paths based on your system:
#   - Fedora: /usr/x86_64-w64-mingw32/sys-root/mingw
#   - Debian: /usr/x86_64-w64-mingw32
#   - MSYS2 cross: /opt/msys64/mingw64
set(CMAKE_FIND_ROOT_PATH
    /usr/${TOOLCHAIN_PREFIX}
    /usr/x86_64-w64-mingw32/sys-root/mingw
    $ENV{MINGW_PREFIX}
)

# Search for programs in the host environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config for cross-compilation
set(PKG_CONFIG_EXECUTABLE ${TOOLCHAIN_PREFIX}-pkg-config)
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_FIND_ROOT_PATH}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_FIND_ROOT_PATH}")

# Windows-specific settings
set(WIN32 TRUE)
set(MINGW TRUE)

# Default to static linking for easier distribution
# Comment this out if you want dynamic linking
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# Output file extensions
set(CMAKE_EXECUTABLE_SUFFIX ".exe")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".dll")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")

# Disable features that don't work on Windows
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries")

# Qt/GTK specific (if using qmake)
# set(QT_QMAKE_EXECUTABLE ${TOOLCHAIN_PREFIX}-qmake-qt5)
