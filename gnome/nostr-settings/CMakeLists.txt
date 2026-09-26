# gnome/nostr-settings/CMakeLists.txt
#
# org.nostr.Settings — libadwaita preferences app over the desktop Nostr
# services (bead nostrc-janr). See README.md.
#
# Requires: glib/gio >= 2.76, json-glib, libsoup-3, gtk4 >= 4.14,
#           libadwaita-1 >= 1.5, libnostr-publish, libnostr, nip19.
# Optional: gnostr-secret (libsecret) for the keyring identity list.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

set(NOSTR_SETTINGS_VERSION 0.1.0)

foreach(_nss_dep nostr-publish nostr nip19)
  if (NOT TARGET ${_nss_dep})
    message(FATAL_ERROR "nostr-settings needs the in-tree '${_nss_dep}' target")
  endif()
endforeach()

pkg_check_modules(NSS_GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.76 gio-2.0 gobject-2.0)
pkg_check_modules(NSS_JSON REQUIRED IMPORTED_TARGET json-glib-1.0>=1.6)
pkg_check_modules(NSS_SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(NSS_ADW  REQUIRED IMPORTED_TARGET gtk4>=4.14 libadwaita-1>=1.5 gio-unix-2.0)

# Pin the API surface to the oldest supported stack (Ubuntu 24.04: GLib
# 2.80, GTK 4.14, libadwaita 1.5) so newer dev headers cannot leak newer
# symbols in, and AdwPreferencesWindow (deprecated in 1.6) stays warning-free.
set(NSS_API_PINS
  GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_76
  GLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_80
  GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14
  GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14
  ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5
  ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5)
set(NSS_WARNINGS -Wall -Wextra -Werror -Wformat=2 -Wno-unused-parameter
                 -Wno-missing-field-initializers)

# ---- GTK-free core (everything the tests exercise) ----
add_library(nostr-settings-core STATIC
  src/core/nss-config.c
  src/core/nss-lists.c
  src/core/nss-nip11.c
  src/core/nss-net.c
  src/core/nss-systemd.c
  src/core/nss-relay-stats.c
  src/core/nss-wallet.c
  src/core/nss-identity.c)
target_include_directories(nostr-settings-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_definitions(nostr-settings-core PUBLIC ${NSS_API_PINS}
  NSS_VERSION="${NOSTR_SETTINGS_VERSION}")
target_compile_options(nostr-settings-core PRIVATE ${NSS_WARNINGS})
target_link_libraries(nostr-settings-core PUBLIC
  nostr-publish nostr nip19
  PkgConfig::NSS_GLIB PkgConfig::NSS_JSON PkgConfig::NSS_SOUP)
if (TARGET gnostr-secret)
  target_link_libraries(nostr-settings-core PUBLIC gnostr-secret)
  target_compile_definitions(nostr-settings-core PUBLIC NSS_HAVE_SECRET=1)
else()
  message(STATUS "nostr-settings: gnostr-secret (libsecret) not available; "
                 "the Identity page will not list keyring identities")
endif()

# ---- App ----
add_executable(nostr-settings
  src/ui/main.c
  src/ui/nss-window.c
  src/ui/nss-widgets.c
  src/ui/nss-page-identity.c
  src/ui/nss-page-relays.c
  src/ui/nss-page-notifications.c
  src/ui/nss-page-wallet.c
  src/ui/nss-page-media.c
  src/ui/nss-page-files.c)
target_include_directories(nostr-settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_compile_options(nostr-settings PRIVATE ${NSS_WARNINGS})
target_link_libraries(nostr-settings PRIVATE nostr-settings-core PkgConfig::NSS_ADW)
install(TARGETS nostr-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- Data ----
set(NSS_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
configure_file(data/org.nostr.Settings.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Settings.desktop @ONLY)
configure_file(data/org.nostr.Settings.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Settings.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Settings.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES data/org.nostr.Settings.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nostr-settings)

# Compiled schema in the build tree so `nostr-settings` runs uninstalled
# (GSETTINGS_SCHEMA_DIR=<build>/gnome/nostr-settings).
find_program(NSS_GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if (NSS_GLIB_COMPILE_SCHEMAS)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/data/org.nostr.Settings.gschema.xml
            ${CMAKE_CURRENT_BINARY_DIR}/
    COMMAND ${NSS_GLIB_COMPILE_SCHEMAS} --strict ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS data/org.nostr.Settings.gschema.xml VERBATIM)
  add_custom_target(nostr-settings-schema ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled)
endif()

find_program(NSS_DESKTOP_FILE_VALIDATE desktop-file-validate)

if (BUILD_TESTING)
  add_subdirectory(tests)
endif()
