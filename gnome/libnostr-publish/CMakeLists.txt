# gnome/libnostr-publish/CMakeLists.txt
#
# Shared relay-side publish core for GNOME Nostr daemons: signer proxy
# (org.nostr.Signer), libsoup-3 NIP-01 WebSocket transport, NIP-65
# target resolution, multi-relay OK aggregation. First consumer:
# gnome/nostr-dav. Planned: nostr-share (nostrc-1xak), nostr-wallet-agent
# (nostrc-yka8).
#
# Always built as a versioned shared library (libnostr-publish.so.0) so
# the daemons share one copy instead of each static-linking it.
#
# Requires: glib-2.0 >= 2.60, gio-2.0, libsoup-3.0, json-glib-1.0
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

pkg_check_modules(NP_GLIB     REQUIRED IMPORTED_TARGET glib-2.0>=2.60 gobject-2.0 gio-2.0)
pkg_check_modules(NP_SOUP     REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(NP_JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

set(NOSTR_PUBLISH_VERSION   0.1.0)
set(NOSTR_PUBLISH_SOVERSION 0)

set(NP_WARNING_FLAGS -Wall -Wextra -Werror -Wno-misleading-indentation)

add_library(nostr-publish SHARED
  src/nostr-publish-policy.c
  src/nostr-publish-signer.c
  src/nostr-publish-signer-dbus.c
  src/nostr-publish-transport.c
  src/nostr-publisher.c
)

set_target_properties(nostr-publish PROPERTIES
  VERSION               ${NOSTR_PUBLISH_VERSION}
  SOVERSION             ${NOSTR_PUBLISH_SOVERSION}
  C_VISIBILITY_PRESET   hidden
  POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(nostr-publish PRIVATE NOSTR_PUBLISH_COMPILATION=1)
target_compile_options(nostr-publish PRIVATE ${NP_WARNING_FLAGS})

# Consumers include <nostr-publish/nostr-publish.h>; the library's own
# sources include sibling headers by bare name.
target_include_directories(nostr-publish
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-publish
)

# GLib/GIO appear in the public headers; libsoup and json-glib do not.
target_link_libraries(nostr-publish
  PUBLIC  PkgConfig::NP_GLIB
  PRIVATE PkgConfig::NP_SOUP PkgConfig::NP_JSONGLIB
)

# --- Install rules ---

configure_file(nostr-publish.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/nostr-publish.pc @ONLY)

install(TARGETS nostr-publish
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/nostr-publish
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nostr-publish.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# --- Tests ---

if (BUILD_TESTING)
  foreach(_np_test IN ITEMS policy publisher)
    add_executable(test-nostr-publish-${_np_test} tests/test_${_np_test}.c)
    target_link_libraries(test-nostr-publish-${_np_test} PRIVATE
      nostr-publish PkgConfig::NP_JSONGLIB)
    target_compile_options(test-nostr-publish-${_np_test} PRIVATE
      ${NP_WARNING_FLAGS})
    add_test(NAME nostr-publish-${_np_test}
             COMMAND test-nostr-publish-${_np_test})
  endforeach()
endif()
