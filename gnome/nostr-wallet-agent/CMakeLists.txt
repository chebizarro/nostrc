# gnome/nostr-wallet-agent/CMakeLists.txt
#
# nostr-wallet-agent: session D-Bus service org.nostr.Wallet1 (NIP-47
# Nostr Wallet Connect) with per-app budgets, libadwaita approval dialogs and
# lightning: / bitcoin: / nostr+walletconnect: scheme handlers (nostrc-yka8).
#
# Requires: glib-2.0 >= 2.74, gio(-unix), json-glib-1.0, gtk4, libadwaita-1 >= 1.4,
#           libsecret-1, gnome/libnostr-publish, libnostr + nip04 + nip44.
# Build:    cmake -B _build -DENABLE_NOSTR_WALLET_AGENT=ON && cmake --build _build
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

pkg_check_modules(NWA_GLIB   REQUIRED IMPORTED_TARGET glib-2.0>=2.74 gobject-2.0 gio-2.0)
pkg_check_modules(NWA_JSON   REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(NWA_ADW    REQUIRED IMPORTED_TARGET gtk4 libadwaita-1>=1.4)
pkg_check_modules(NWA_SECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(NWA_GIOUNIX QUIET IMPORTED_TARGET gio-unix-2.0)

foreach(_dep nostr nip04 nostr_nip44_core nostr_json)
  if (NOT TARGET ${_dep})
    message(FATAL_ERROR "nostr-wallet-agent needs the in-tree '${_dep}' target "
                        "(configure from the repository root with ENABLE_NIP04/ENABLE_NIP44).")
  endif()
endforeach()

if (NOT TARGET nostr-publish)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libnostr-publish
                   ${CMAKE_CURRENT_BINARY_DIR}/libnostr-publish)
endif()
if (NOT TARGET gnostr-secret)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../seahorse
                   ${CMAKE_CURRENT_BINARY_DIR}/seahorse)
endif()

set(NWA_VERSION 0.1.0)

set(NWA_WARNING_FLAGS -Wall -Wextra -Werror -Wformat=2 -Wno-misleading-indentation
                      -Wno-deprecated-declarations)

# --- D-Bus introspection XML embedded as a C string ---
set(NWA_DBUS_XML ${CMAKE_CURRENT_SOURCE_DIR}/../dbus/org.nostr.Wallet1.xml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NWA_DBUS_XML})
file(READ ${NWA_DBUS_XML} _nwa_xml)
string(REPLACE "\\" "\\\\" _nwa_xml "${_nwa_xml}")
string(REPLACE "\"" "\\\"" _nwa_xml "${_nwa_xml}")
string(REPLACE "\n" "\\n\"\n\"" _nwa_xml "${_nwa_xml}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/nwa-dbus-xml.h
     "/* generated from gnome/dbus/org.nostr.Wallet1.xml */\n"
     "static const char nwa_dbus_xml[] =\n\"${_nwa_xml}\";\n")

# --- NIP-47 core (URI parser, envelope, NIP-04/NIP-44 helpers) ---
# nostr_nip47_core follows BUILD_SHARED_LIBS and is not installed, so the
# three translation units the agent needs are compiled in statically.
set(_nip47 ${CMAKE_CURRENT_SOURCE_DIR}/../../nips/nip47)
add_library(nwa-nip47 STATIC
  ${_nip47}/src/core/nwc_uri.c
  ${_nip47}/src/core/nwc_envelope.c
  ${_nip47}/src/core/nwc_client.c
)
# nip04 does not export its include directory; nip44 does.
target_include_directories(nwa-nip47 PUBLIC ${_nip47}/include ${_nip47}/../nip04/include)
target_link_libraries(nwa-nip47 PUBLIC nostr nostr_json nostr_nip44_core nip04)
set_target_properties(nwa-nip47 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Core (shared by the daemon and the tests; no GTK) ---
add_library(nostr-wallet-agent-core STATIC
  src/nwa-error.c
  src/nwa-bolt11.c
  src/nwa-uri.c
  src/nwa-budget.c
  src/nwa-policy.c
  src/nwa-caller.c
  src/nwa-nwc.c
)
target_include_directories(nostr-wallet-agent-core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(nostr-wallet-agent-core PUBLIC
  nwa-nip47
  nostr-publish
  PkgConfig::NWA_GLIB
  PkgConfig::NWA_JSON
)
if (NWA_GIOUNIX_FOUND)
  target_link_libraries(nostr-wallet-agent-core PUBLIC PkgConfig::NWA_GIOUNIX)
endif()
target_compile_options(nostr-wallet-agent-core PRIVATE ${NWA_WARNING_FLAGS})
# The only executable allowed to act for a web origin (*For methods): the
# browser bridge from apps/gnostr-signer/native-host, installed by the same
# tree with the same libexecdir.
target_compile_definitions(nostr-wallet-agent-core PRIVATE
  NWA_WEBEXT_HOST_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/nostr-signer-webext-host")
# Test hook: honour NOSTR_WALLET_AGENT_ORIGIN_BRIDGES so CTest can run the
# build-tree bridge. Off in package builds (BUILD_TESTING=OFF).
option(NOSTR_WALLET_AGENT_ORIGIN_BRIDGE_ENV
       "Let NOSTR_WALLET_AGENT_ORIGIN_BRIDGES replace the browser bridge path (tests only)"
       ${BUILD_TESTING})
if (NOSTR_WALLET_AGENT_ORIGIN_BRIDGE_ENV)
  # PUBLIC: test_caller checks which behaviour it links against.
  target_compile_definitions(nostr-wallet-agent-core PUBLIC NWA_ORIGIN_BRIDGE_ENV=1)
endif()

# --- Daemon ---
add_executable(nostr-wallet-agent
  src/main.c
  src/nwa-service.c
  src/nwa-ui.c
)
target_compile_definitions(nostr-wallet-agent PRIVATE
  NWA_HAVE_LIBSECRET=1
  NWA_VERSION="${NWA_VERSION}")
target_link_libraries(nostr-wallet-agent PRIVATE
  nostr-wallet-agent-core
  gnostr-secret
  PkgConfig::NWA_ADW
  PkgConfig::NWA_SECRET
)
target_compile_options(nostr-wallet-agent PRIVATE ${NWA_WARNING_FLAGS})

# --- Install ---
pkg_get_variable(NWA_SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
if (NOT NWA_SYSTEMD_USER_UNIT_DIR)
  set(NWA_SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user")
endif()

configure_file(data/nostr-wallet-agent.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/nostr-wallet-agent.service @ONLY)
configure_file(data/org.nostr.Wallet1.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Wallet1.service @ONLY)
configure_file(data/org.nostr.Wallet.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Wallet.desktop @ONLY)

install(TARGETS nostr-wallet-agent DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nostr-wallet-agent.service
        DESTINATION "${NWA_SYSTEMD_USER_UNIT_DIR}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Wallet1.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Wallet.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/org.nostr.Wallet.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES ${NWA_DBUS_XML}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)
install(FILES README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-wallet-agent)

# --- Tests (no network, no display, no keyring) ---
if (BUILD_TESTING)
  foreach(_t IN ITEMS uri budget policy nwc caller)
    add_executable(test-nostr-wallet-agent-${_t} tests/test_${_t}.c)
    target_link_libraries(test-nostr-wallet-agent-${_t} PRIVATE nostr-wallet-agent-core)
    target_compile_options(test-nostr-wallet-agent-${_t} PRIVATE ${NWA_WARNING_FLAGS})
    add_test(NAME nostr-wallet-agent-${_t} COMMAND test-nostr-wallet-agent-${_t})
  endforeach()

  # A NIP-47 wallet service + relay in one process on 127.0.0.1, for tests
  # that run the real agent binary paired (here and in the browser bridge's
  # WebLN e2e, apps/gnostr-signer/native-host). Needs the test pairing hook.
  pkg_check_modules(NWA_SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
  pkg_check_modules(NWA_GIOUNIX_T REQUIRED IMPORTED_TARGET gio-unix-2.0)
  add_executable(nwa-fixture-wallet tests/nwa-fixture-wallet.c)
  target_link_libraries(nwa-fixture-wallet PRIVATE nostr-wallet-agent-core
                        PkgConfig::NWA_SOUP PkgConfig::NWA_GIOUNIX_T)
  target_compile_options(nwa-fixture-wallet PRIVATE ${NWA_WARNING_FLAGS})
  find_program(NWA_DESKTOP_VALIDATE desktop-file-validate)
  if (NWA_DESKTOP_VALIDATE)
    add_test(NAME nostr-wallet-agent-desktop-file
             COMMAND ${NWA_DESKTOP_VALIDATE} ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Wallet.desktop)
  endif()
  find_program(NWA_GLIB_COMPILE_SCHEMAS glib-compile-schemas)
  if (NWA_GLIB_COMPILE_SCHEMAS)
    add_test(NAME nostr-wallet-agent-gschema
             COMMAND ${NWA_GLIB_COMPILE_SCHEMAS} --strict --dry-run ${CMAKE_CURRENT_SOURCE_DIR}/data)
  endif()
endif()
