# tests/testkit/CMakeLists.txt — Shared test infrastructure library
#
# Builds a static library that test binaries across all subprojects
# can link against for common fixtures, NDB setup, event generation,
# main-loop helpers, and memory measurement.
#
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB2   REQUIRED IMPORTED_TARGET glib-2.0>=2.70)
pkg_check_modules(GIO2    REQUIRED IMPORTED_TARGET gio-2.0>=2.70)

add_library(gnostr-testkit STATIC
  gnostr-testkit.c
)

target_include_directories(gnostr-testkit
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Testkit links against nostr-gobject (for storage_ndb functions)
# and GLib/GIO for main loop and file system utilities.
target_link_libraries(gnostr-testkit
  PUBLIC
    nostr_gobject
    PkgConfig::GLIB2
    PkgConfig::GIO2
)

# On macOS, link against CoreFoundation for mach_task_basic_info
if(APPLE)
  target_link_libraries(gnostr-testkit PUBLIC "-framework CoreFoundation")
endif()
