# gnome/nostr-dispatcher — kind-aware router for nostr: / web+nostr: links
# (org.nostr.Dispatcher1, bead nostrc-1v65). Enabled from the top level with
# -DENABLE_NOSTR_DISPATCHER=ON. Headless: GLib/GIO, json-glib, libsoup-3
# (session-relay socket), libnostr (validation + relay-hint fetches). No
# GTK / libadwaita.
cmake_minimum_required(VERSION 3.20)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ND_DEPS REQUIRED IMPORTED_TARGET
  glib-2.0>=2.70 gio-2.0 gio-unix-2.0 json-glib-1.0 libsoup-3.0)

option(NOSTR_DISPATCHER_BUILD_TESTS "Build nostr-dispatcher unit tests" ${BUILD_TESTING})

# NIP-19 codec: the shared libnip19.so.0 (SONAME, shipped once in
# libnostr-nips0 / libnostr-nips since nostrc-4c0o; nostrc-9rvm dropped the
# private static copy).
if(NOT TARGET nip19)
  message(FATAL_ERROR "nostr-dispatcher requires ENABLE_NIP19=ON")
endif()

# Introspection XML: gnome/dbus/ is the single source of truth; embed it.
set(ND_IFACE_XML ${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.Dispatcher1.xml)
set(ND_HANDLER_XML ${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.Handler1.xml
                   ${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.Handler2.xml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ND_IFACE_XML})
file(READ ${ND_IFACE_XML} _nd_xml)
string(REPLACE "\\" "\\\\" _nd_xml "${_nd_xml}")
string(REPLACE "\"" "\\\"" _nd_xml "${_nd_xml}")
string(REPLACE "\n" "\\n\"\n  \"" _nd_xml "${_nd_xml}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/nd-introspection.h
  "/* Generated from gnome/dbus/org.nostr.Dispatcher1.xml. */\n"
  "static const char ND_DISPATCHER1_XML[] =\n  \"${_nd_xml}\";\n")

# Portable core (no GDesktopAppInfo): URI/TLV parsing, kind grammar, event
# admission, fetch. Builds and unit-tests on any GLib platform.
add_library(nd_portable STATIC
  src/nd-kinds.c
  src/nd-uri.c
  src/nd-event.c
  src/nd-fetch.c
  src/nd-nip89.c)
target_include_directories(nd_portable PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/libgo/include)
target_compile_options(nd_portable PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nd_portable PUBLIC PkgConfig::ND_DEPS nip19 nostr)

set(ND_PORTABLE_TESTS kinds uri event nip89)
if(NOSTR_DISPATCHER_BUILD_TESTS)
  foreach(t ${ND_PORTABLE_TESTS})
    add_executable(test_nd_${t} tests/test_nd_${t}.c)
    target_compile_options(test_nd_${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(test_nd_${t} PRIVATE nd_portable)
    add_test(NAME nostr_dispatcher_${t} COMMAND test_nd_${t})
    set_tests_properties(nostr_dispatcher_${t} PROPERTIES
      LABELS "nostr-dispatcher" TIMEOUT 60)
  endforeach()
endif()

# Registry + launching need GDesktopAppInfo (freedesktop platforms only).
find_path(ND_GDESKTOPAPPINFO_H gio/gdesktopappinfo.h HINTS ${ND_DEPS_INCLUDE_DIRS})
if(NOT ND_GDESKTOPAPPINFO_H)
  message(WARNING "nostr-dispatcher: GDesktopAppInfo unavailable on this "
                  "platform; building portable core + tests only")
  return()
endif()

add_library(nd_core STATIC
  src/nd-registry.c
  src/nd-dispatch.c)
target_compile_options(nd_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nd_core PUBLIC nd_portable)

add_executable(nostr-dispatcher src/nostr-dispatcher.c)
target_include_directories(nostr-dispatcher PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(nostr-dispatcher PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nostr-dispatcher PRIVATE nd_core)

# ---- data -----------------------------------------------------------------
set(ND_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
configure_file(data/org.nostr.Dispatcher.desktop.in
  ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dispatcher.desktop @ONLY)
configure_file(data/nostr-dispatcher.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/nostr-dispatcher.service @ONLY)
configure_file(data/org.nostr.Dispatcher1.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dispatcher1.service @ONLY)
configure_file(data/org.nostr.Dispatcher.service.in
  ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dispatcher.service @ONLY)

pkg_get_variable(ND_SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
if(NOT ND_SYSTEMD_USER_UNIT_DIR)
  set(ND_SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
endif()

install(TARGETS nostr-dispatcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dispatcher.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/nostr.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES data/handlers.list DESTINATION ${CMAKE_INSTALL_DATADIR}/nostr)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dispatcher1.service
              ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Dispatcher.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${ND_IFACE_XML} ${ND_HANDLER_XML}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nostr-dispatcher.service
        DESTINATION ${ND_SYSTEMD_USER_UNIT_DIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-dispatcher)
install(FILES doc/nostr-dispatcher.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# ---- tests ----------------------------------------------------------------
if(NOSTR_DISPATCHER_BUILD_TESTS)
  # test_nd_registry_gio: real .desktop enumeration through private XDG
  # dirs + dry-run dispatch.
  foreach(t registry registry_gio)
    add_executable(test_nd_${t} tests/test_nd_${t}.c)
    target_compile_options(test_nd_${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(test_nd_${t} PRIVATE nd_core)
    add_test(NAME nostr_dispatcher_${t} COMMAND test_nd_${t})
    set_tests_properties(nostr_dispatcher_${t} PROPERTIES
      LABELS "nostr-dispatcher" TIMEOUT 60)
  endforeach()
endif()
