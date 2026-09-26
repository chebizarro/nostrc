# gnome/nostr-share/CMakeLists.txt
#
# nostr-share: "Share to Nostr" CLI + libadwaita dialog, registered for
# Open With via org.nostr.Share.desktop (bead nostrc-1xak).
#
# Requires: glib/gio >= 2.70, json-glib, libsoup-3, libnostr-publish,
#           libhanami (Blossom), nip34, nip19, libnostr.
# Optional: gtk4 + libadwaita-1 >= 1.4 for the dialog (NOSTR_SHARE_UI).
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

set(NOSTR_SHARE_VERSION 0.1.0)

foreach(_ns_dep nostr-publish hanami nip34 nip19 nostr)
  if (NOT TARGET ${_ns_dep})
    message(FATAL_ERROR
      "nostr-share needs the '${_ns_dep}' target (enable BUILD_LIBHANAMI and "
      "ENABLE_NOSTR_SHARE together)")
  endif()
endforeach()

pkg_check_modules(NS_GLIB     REQUIRED IMPORTED_TARGET glib-2.0>=2.70 gio-2.0 gobject-2.0)
pkg_check_modules(NS_JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0>=1.6)
pkg_check_modules(NS_SOUP     REQUIRED IMPORTED_TARGET libsoup-3.0)

option(NOSTR_SHARE_UI "Build the libadwaita share dialog (auto-disabled if gtk4/libadwaita are missing)" ON)
set(_ns_have_ui OFF)
if (NOSTR_SHARE_UI)
  pkg_check_modules(NS_ADW IMPORTED_TARGET gtk4>=4.10 libadwaita-1>=1.4)
  if (NS_ADW_FOUND)
    set(_ns_have_ui ON)
  else()
    message(STATUS "nostr-share: gtk4/libadwaita-1 >= 1.4 not found; CLI only")
  endif()
endif()

set(NS_WARNING_FLAGS -Wall -Wextra -Werror -Wno-misleading-indentation
                     -Wno-unused-parameter)

add_library(nostr-share-core STATIC
  src/ns-kind.c
  src/ns-event.c
  src/ns-strip.c
  src/ns-config.c
  src/ns-net.c
  src/ns-blossom.c
  src/ns-git.c
  src/ns-dav.c
  src/ns-share.c
)
target_include_directories(nostr-share-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(nostr-share-core PUBLIC NS_VERSION="${NOSTR_SHARE_VERSION}")
target_compile_options(nostr-share-core PRIVATE ${NS_WARNING_FLAGS})
target_link_libraries(nostr-share-core PUBLIC
  nostr-publish hanami nip34 nip19 nostr
  PkgConfig::NS_GLIB PkgConfig::NS_JSONGLIB PkgConfig::NS_SOUP)

set(_ns_sources src/main.c)
if (_ns_have_ui)
  list(APPEND _ns_sources src/ns-dialog.c)
endif()
add_executable(nostr-share ${_ns_sources})
target_compile_options(nostr-share PRIVATE ${NS_WARNING_FLAGS})
target_link_libraries(nostr-share PRIVATE nostr-share-core)
if (_ns_have_ui)
  target_compile_definitions(nostr-share PRIVATE NS_HAVE_UI=1)
  target_link_libraries(nostr-share PRIVATE PkgConfig::NS_ADW)
endif()

install(TARGETS nostr-share RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/org.nostr.Share.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/nostr-share.conf.example README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nostr-share)

if (BUILD_TESTING)
  foreach(_ns_test IN ITEMS kind event strip share)
    add_executable(test-nostr-share-${_ns_test} tests/test_${_ns_test}.c)
    target_link_libraries(test-nostr-share-${_ns_test} PRIVATE nostr-share-core)
    target_compile_options(test-nostr-share-${_ns_test} PRIVATE ${NS_WARNING_FLAGS})
    add_test(NAME nostr-share-${_ns_test} COMMAND test-nostr-share-${_ns_test})
    set_tests_properties(nostr-share-${_ns_test} PROPERTIES
      TIMEOUT 60 LABELS "nostr-share;unit"
      ENVIRONMENT "NOSTR_SHARE_SESSION_RELAY_SOCKET=/nonexistent;NOSTR_SHARE_CONFIG=/nonexistent")
  endforeach()
  find_program(_ns_dfv desktop-file-validate)
  if (_ns_dfv)
    add_test(NAME nostr-share-desktop-file
             COMMAND ${_ns_dfv} ${CMAKE_CURRENT_SOURCE_DIR}/data/org.nostr.Share.desktop)
  endif()
endif()
