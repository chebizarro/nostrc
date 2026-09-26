# gnome/nostr-search-provider — GNOME Shell search provider for Nostr
# profiles and notes held by the per-user session relay
# (org.gnome.Shell.SearchProvider2, bead nostrc-hwwn). Enabled from the top
# level with -DENABLE_NOSTR_SEARCH_PROVIDER=ON. Headless: GLib/GIO,
# json-glib, libsoup-3, jansson, libnostr; gdk-pixbuf optional (avatars).
# No GTK / libadwaita.
cmake_minimum_required(VERSION 3.20)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NSP_DEPS REQUIRED IMPORTED_TARGET
  glib-2.0>=2.66 gio-2.0 gio-unix-2.0 json-glib-1.0 libsoup-3.0 jansson)
pkg_check_modules(NSP_PIXBUF IMPORTED_TARGET gdk-pixbuf-2.0)

option(NOSTR_SEARCH_PROVIDER_BUILD_TESTS "Build nostr-search-provider unit tests" ${BUILD_TESTING})

set(NSP_WARN -Wall -Wextra -Wno-unused-parameter -Werror=format -Werror=format-security)

# NIP-19 codec, linked statically (same reasoning as nostr-dispatcher:
# libnip19.so is not a packaged library).
file(GLOB NSP_NIP19_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/nips/nip19/src/*.c)
add_library(nsp_nip19 STATIC ${NSP_NIP19_SOURCES})
set_target_properties(nsp_nip19 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nsp_nip19
  PUBLIC  ${CMAKE_SOURCE_DIR}/nips/nip19/include
  PRIVATE ${CMAKE_SOURCE_DIR}/libnostr/include ${CMAKE_SOURCE_DIR}/libgo/include)
target_link_libraries(nsp_nip19 PUBLIC nostr)

# Reused, compiled in (not linked, so neither package depends on the other):
#  - nostr-dispatcher's NIP-21 parser/encoder and validated-event admission;
#  - nostr-homed's NIP-05 grammar, .well-known parser and TTL cache
#    (libc + jansson only; the broker's fork/exec client is not used).
set(ND_SRC ${CMAKE_SOURCE_DIR}/gnome/nostr-dispatcher/src)
set(NH_DIR ${CMAKE_SOURCE_DIR}/gnome/nostr-homed)

add_library(nsp_core STATIC
  ${ND_SRC}/nd-uri.c
  ${ND_SRC}/nd-event.c
  ${NH_DIR}/src/nip05/nip05_validate.c
  ${NH_DIR}/src/nip05/nip05_cache.c
  src/nsp-text.c
  src/nsp-query.c
  src/nsp-item.c
  src/nsp-relay.c
  src/nsp-http.c
  src/nsp-nip05.c
  src/nsp-avatar.c
  src/nsp-engine.c)
target_include_directories(nsp_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${ND_SRC}
  ${NH_DIR}/include
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/libgo/include)
target_compile_options(nsp_core PRIVATE ${NSP_WARN})
target_link_libraries(nsp_core PUBLIC PkgConfig::NSP_DEPS nsp_nip19 nostr)
if(NSP_PIXBUF_FOUND)
  target_compile_definitions(nsp_core PUBLIC NSP_HAVE_PIXBUF=1)
  target_link_libraries(nsp_core PUBLIC PkgConfig::NSP_PIXBUF)
else()
  message(STATUS "nostr-search-provider: gdk-pixbuf not found; avatars disabled")
endif()

# Introspection XML, embedded as a C string.
set(NSP_IFACE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/org.gnome.Shell.SearchProvider2.xml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NSP_IFACE_XML})
file(READ ${NSP_IFACE_XML} _nsp_xml)
string(REPLACE "\\" "\\\\" _nsp_xml "${_nsp_xml}")
string(REPLACE "\"" "\\\"" _nsp_xml "${_nsp_xml}")
string(REPLACE "\n" "\\n\"\n  \"" _nsp_xml "${_nsp_xml}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/nsp-introspection.h
  "/* Generated from data/org.gnome.Shell.SearchProvider2.xml. */\n"
  "static const char NSP_SEARCH_PROVIDER2_XML[] =\n  \"${_nsp_xml}\";\n")

add_executable(nostr-search-provider src/nostr-search-provider.c)
target_include_directories(nostr-search-provider PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(nostr-search-provider PRIVATE ${NSP_WARN})
target_link_libraries(nostr-search-provider PRIVATE nsp_core)
find_path(NSP_GDESKTOPAPPINFO_H gio/gdesktopappinfo.h HINTS ${NSP_DEPS_INCLUDE_DIRS})
if(NSP_GDESKTOPAPPINFO_H)
  target_compile_definitions(nostr-search-provider PRIVATE NSP_HAVE_GDESKTOPAPPINFO=1)
endif()

# ---- data -----------------------------------------------------------------
set(NSP_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
foreach(f org.nostr.SearchProvider.desktop org.nostr.SearchProvider.service
          nostr-search-provider.service)
  configure_file(data/${f}.in ${CMAKE_CURRENT_BINARY_DIR}/${f} @ONLY)
endforeach()

pkg_get_variable(NSP_SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
if(NOT NSP_SYSTEMD_USER_UNIT_DIR)
  set(NSP_SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
endif()

install(TARGETS nostr-search-provider RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/org.nostr.SearchProvider.ini
        DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/search-providers)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.SearchProvider.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.SearchProvider.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nostr-search-provider.service
        DESTINATION ${NSP_SYSTEMD_USER_UNIT_DIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-search-provider)

# ---- tests (no network: mock relay on a private AF_UNIX socket) ------------
if(NOSTR_SEARCH_PROVIDER_BUILD_TESTS)
  add_library(nsp_testutil STATIC tests/nsp-testutil.c)
  target_compile_options(nsp_testutil PRIVATE ${NSP_WARN})
  target_link_libraries(nsp_testutil PUBLIC nsp_core)
  foreach(t query meta engine net)
    add_executable(test_nsp_${t} tests/test_nsp_${t}.c)
    target_compile_options(test_nsp_${t} PRIVATE ${NSP_WARN})
    target_link_libraries(test_nsp_${t} PRIVATE nsp_testutil)
    add_test(NAME nostr_search_provider_${t} COMMAND test_nsp_${t})
    set_tests_properties(nostr_search_provider_${t} PROPERTIES
      LABELS "nostr-search-provider" TIMEOUT 60
      ENVIRONMENT "NOSTR_SEARCH_PROVIDER_NO_NETWORK=1")
  endforeach()
  # D-Bus end-to-end: the real daemon on a private bus (skips without
  # dbus-daemon).
  add_executable(test_nsp_dbus tests/test_nsp_dbus.c)
  target_compile_options(test_nsp_dbus PRIVATE ${NSP_WARN})
  target_compile_definitions(test_nsp_dbus PRIVATE
    NSP_BINARY="$<TARGET_FILE:nostr-search-provider>"
    ND_DISPATCHER_XML="${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.Dispatcher1.xml")
  target_link_libraries(test_nsp_dbus PRIVATE nsp_testutil)
  add_dependencies(test_nsp_dbus nostr-search-provider)
  add_test(NAME nostr_search_provider_dbus COMMAND test_nsp_dbus)
  set_tests_properties(nostr_search_provider_dbus PROPERTIES
    LABELS "nostr-search-provider" TIMEOUT 60)
endif()
