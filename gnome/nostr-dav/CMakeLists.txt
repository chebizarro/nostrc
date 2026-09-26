# gnome/nostr-dav/CMakeLists.txt
#
# Localhost CalDAV/CardDAV/WebDAV bridge for Nostr ↔ GNOME desktop integration.
#
# Requires: glib-2.0 >= 2.60, libsoup-3.0, libxml-2.0, json-glib-1.0, sqlite3
# Optional: libsecret-1 (mirrors the bearer token into the user keyring)
#
# Build: cmake -B _build -DENABLE_NOSTR_DAV=ON && cmake --build _build
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

# Required deps (2.60: G_APPLICATION_ALLOW_REPLACEMENT; sqlite 3.24: UPSERT)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.60 gobject-2.0 gio-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(JSONLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3>=3.24)

# Optional deps
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)

set(ND_WARNING_FLAGS -Wall -Wextra -Werror -Wno-misleading-indentation)

# --- Core (shared by the daemon and the tests) ---

add_library(nostr-dav-core STATIC
  src/nd-dav-server.c
  src/nd-token-store.c
  src/nd-store-db.c
  src/nd-config.c
  src/nd-ical.c
  src/nd-calendar-store.c
  src/nd-vcard.c
  src/nd-contact-store.c
  src/nd-file-entry.c
  src/nd-file-store.c
  src/nd-signer.c
  src/nd-signer-dbus.c
  src/nd-relay-transport.c
  src/nd-relay-sync.c
  src/nd-publisher.c
)

target_include_directories(nostr-dav-core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nostr-dav-core PUBLIC
  PkgConfig::GLIB
  PkgConfig::SOUP
  PkgConfig::LIBXML2
  PkgConfig::JSONLIB
  PkgConfig::SQLITE3
)

target_compile_options(nostr-dav-core PRIVATE ${ND_WARNING_FLAGS})

if (LIBSECRET_FOUND)
  target_compile_definitions(nostr-dav-core PRIVATE HAVE_LIBSECRET=1)
  target_link_libraries(nostr-dav-core PRIVATE PkgConfig::LIBSECRET)
  message(STATUS "nostr-dav: libsecret found — new tokens are mirrored to the keyring")
else()
  message(STATUS "nostr-dav: libsecret not found — token file only")
endif()

# --- nostr-dav daemon ---

add_executable(nostr-dav
  src/nd-main.c
  src/nd-application.c
)
target_link_libraries(nostr-dav PRIVATE nostr-dav-core)
target_compile_options(nostr-dav PRIVATE ${ND_WARNING_FLAGS})

# --- Install rules ---

pkg_get_variable(SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
if (NOT SYSTEMD_USER_UNIT_DIR)
  set(SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
endif()

configure_file(systemd/nostr-dav.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/nostr-dav.service @ONLY)
configure_file(systemd/org.nostr.Dav.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dav.service @ONLY)

install(TARGETS nostr-dav DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nostr-dav.service
              systemd/nostr-dav-dirs.service
        DESTINATION "${SYSTEMD_USER_UNIT_DIR}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dav.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES config/nostr-dav.conf.sample
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-dav)

# --- Tests ---

if (BUILD_TESTING)
  add_library(nostr-dav-test-harness STATIC tests/nd-test-harness.c)
  target_include_directories(nostr-dav-test-harness PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(nostr-dav-test-harness PUBLIC nostr-dav-core)
  target_compile_options(nostr-dav-test-harness PRIVATE ${ND_WARNING_FLAGS})

  foreach(_nd_test IN ITEMS dav_propfind auth_required store_sqlite
                            relay_sync publish_rollback)
    string(REPLACE "_" "-" _nd_test_dash ${_nd_test})
    add_executable(test-nostr-dav-${_nd_test_dash} tests/test_${_nd_test}.c)
    target_link_libraries(test-nostr-dav-${_nd_test_dash} PRIVATE
      nostr-dav-test-harness)
    target_compile_options(test-nostr-dav-${_nd_test_dash} PRIVATE
      ${ND_WARNING_FLAGS})
    add_test(NAME nostr-dav-${_nd_test_dash}
             COMMAND test-nostr-dav-${_nd_test_dash})
  endforeach()
endif()
