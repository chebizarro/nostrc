cmake_minimum_required(VERSION 3.10)
project(gnostr-signer)

include(GNUInstallDirs)

# IDs and binaries
set(SUITE_NAME "GNostr")
set(APP_SIGNER_NAME "GNostr Signer")
set(APP_SIGNER_BIN "gnostr-signer")
set(APP_SIGNER_FLATPAK_ID "org.gnostr.Signer")
set(APP_SIGNER_GSETTINGS "org.gnostr.Signer")
set(DBUS_BUS_NAME "org.nostr.Signer")
set(DBUS_OBJECT_PATH "/org/nostr/signer")
set(DBUS_INTERFACE "org.nostr.Signer")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)

# Main GTK4 application (UI shell)
file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c)
if (NOT APP_SRC)
  set(APP_SRC src/main_app.c)
endif()
add_executable(${APP_SIGNER_BIN} ${APP_SRC})
target_compile_features(${APP_SIGNER_BIN} PRIVATE c_std_11)
target_compile_definitions(${APP_SIGNER_BIN} PRIVATE 
  APP_ID="${APP_SIGNER_FLATPAK_ID}" APP_NAME="${APP_SIGNER_NAME}")
target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::GTK4 PkgConfig::GLIB)

# Daemon providing DBus (55L) and UDS (5F) fallback (skeleton now)
add_executable(gnostr-signer-daemon daemon/main_daemon.c daemon/uds_sockd.c)
target_compile_features(gnostr-signer-daemon PRIVATE c_std_11)
target_compile_definitions(gnostr-signer-daemon PRIVATE 
  DBUS_BUS_NAME="${DBUS_BUS_NAME}" DBUS_OBJECT_PATH="${DBUS_OBJECT_PATH}" DBUS_INTERFACE="${DBUS_INTERFACE}")
# link core libs (libnostr/nip5f) and GLib
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
target_include_directories(gnostr-signer-daemon PRIVATE ${NSYNC_INCLUDE_DIR})
find_library(NSYNC_LIB nsync REQUIRED)
target_link_libraries(gnostr-signer-daemon PRIVATE nostr_nip55l_glib PkgConfig::GLIB ${NSYNC_LIB})

# Data install (schemas, dbus, desktop)
install(TARGETS ${APP_SIGNER_BIN} gnostr-signer-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# GSettings schema
install(FILES data/org.gnostr.Signer.gschema.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/glib-2.0/schemas)
# DBus service (session)
install(FILES data/com.nostr.Signer.service DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/services)
# Desktop file and icons/metainfo can be added later
