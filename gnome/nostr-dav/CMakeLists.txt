# gnome/nostr-dav/CMakeLists.txt
#
# Localhost CalDAV/CardDAV/WebDAV bridge for Nostr ↔ GNOME desktop integration.
#
# Requires: libsoup-3.0, libxml-2.0, glib-2.0
# Optional: libsecret-1 (for persistent bearer token storage)
#
# Build: cmake -B _build -DENABLE_NOSTR_DAV=ON && cmake --build _build
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)

# Required deps
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.56 gobject-2.0 gio-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(JSONLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

# Optional deps
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)

# --- nostr-dav daemon ---

set(ND_SOURCES
  src/nd-main.c
  src/nd-application.c
  src/nd-dav-server.c
  src/nd-token-store.c
  src/nd-ical.c
  src/nd-calendar-store.c
  src/nd-vcard.c
  src/nd-contact-store.c
  src/nd-file-entry.c
  src/nd-file-store.c
)

add_executable(nostr-dav ${ND_SOURCES})

target_include_directories(nostr-dav PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nostr-dav PRIVATE
  PkgConfig::GLIB
  PkgConfig::SOUP
  PkgConfig::LIBXML2
  PkgConfig::JSONLIB
)

if (LIBSECRET_FOUND)
  target_compile_definitions(nostr-dav PRIVATE HAVE_LIBSECRET=1)
  target_link_libraries(nostr-dav PRIVATE PkgConfig::LIBSECRET)
  message(STATUS "nostr-dav: libsecret found — persistent bearer tokens enabled")
else()
  message(STATUS "nostr-dav: libsecret not found — ephemeral tokens only (dev mode)")
endif()

# --- Install rules ---

install(TARGETS nostr-dav DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES systemd/nostr-dav.service
        DESTINATION lib/systemd/user)
install(FILES systemd/org.nostr.Dav.service
        DESTINATION share/dbus-1/services)

# --- Tests ---

if (BUILD_TESTING)
  add_executable(test-nostr-dav
    tests/test_dav_propfind.c
    src/nd-dav-server.c
    src/nd-token-store.c
    src/nd-ical.c
    src/nd-calendar-store.c
    src/nd-vcard.c
    src/nd-contact-store.c
    src/nd-file-entry.c
    src/nd-file-store.c
  )

  target_include_directories(test-nostr-dav PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

  target_link_libraries(test-nostr-dav PRIVATE
    PkgConfig::GLIB
    PkgConfig::SOUP
    PkgConfig::LIBXML2
    PkgConfig::JSONLIB
  )

  if (LIBSECRET_FOUND)
    target_compile_definitions(test-nostr-dav PRIVATE HAVE_LIBSECRET=1)
    target_link_libraries(test-nostr-dav PRIVATE PkgConfig::LIBSECRET)
  endif()

  add_test(NAME nostr-dav-propfind COMMAND test-nostr-dav)
endif()
