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
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)

# Main GTK4 application (UI shell)
file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c src/ui/sheets/*.c src/ui/widgets/*.c)
if (NOT APP_SRC)
  set(APP_SRC src/main_app.c)
endif()
set(APP_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml)
set(APP_GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
# Rebuild resources.c whenever any referenced UI changes
file(GLOB_RECURSE APP_UI_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/data/*.ui)
add_custom_command(
  OUTPUT ${APP_GRESOURCE_C}
  COMMAND glib-compile-resources --target=${APP_GRESOURCE_C} --generate-source --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data ${APP_GRESOURCE_XML}
  DEPENDS ${APP_GRESOURCE_XML} ${APP_UI_FILES}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
  VERBATIM)

add_executable(${APP_SIGNER_BIN} ${APP_SRC} ${APP_GRESOURCE_C})
target_compile_features(${APP_SIGNER_BIN} PRIVATE c_std_11)
target_compile_definitions(${APP_SIGNER_BIN} PRIVATE 
  APP_ID="${APP_SIGNER_FLATPAK_ID}" APP_NAME="${APP_SIGNER_NAME}" APP_RESOURCE_PATH="/org/gnostr/signer")
# NIP-55L core headers for owner/key APIs used by the UI
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip55l/include)
# Link against core to resolve owner APIs
target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::ADWAITA nostr_nip55l_core)

# Include project root so we can include gnome/seahorse/secret_store.h from the app
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR})

# If Seahorse helper lib is built, link it to enable secret deletion helpers
if (TARGET gnostr-secret)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE gnostr-secret)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_LIBSECRET=1)
endif()

# Alternatively, if libsecret was detected directly here, enable the codepath too
if (LIBSECRET_FOUND)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_LIBSECRET=1)
endif()

# Daemon providing DBus (55L) and UDS (5F) fallback (skeleton now)
option(ENABLE_TCP_IPC "Enable TCP IPC backend in gnostr-signer-daemon" OFF)

add_executable(gnostr-signer-daemon daemon/main_daemon.c daemon/uds_sockd.c daemon/ipc.c)
target_compile_features(gnostr-signer-daemon PRIVATE c_std_11)
target_compile_definitions(gnostr-signer-daemon PRIVATE 
  DBUS_BUS_NAME="${DBUS_BUS_NAME}" DBUS_OBJECT_PATH="${DBUS_OBJECT_PATH}" DBUS_INTERFACE="${DBUS_INTERFACE}")
# link core libs (libnostr/nip5f) and GLib
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
target_include_directories(gnostr-signer-daemon PRIVATE ${NSYNC_INCLUDE_DIR})
find_library(NSYNC_LIB nsync REQUIRED)
target_link_libraries(gnostr-signer-daemon PRIVATE nostr_nip55l_glib nostr_nip5f_core PkgConfig::GLIB ${NSYNC_LIB})

if (ENABLE_TCP_IPC)
  target_compile_definitions(gnostr-signer-daemon PRIVATE GNOSTR_ENABLE_TCP_IPC=1)
endif()

# Data install (schemas, dbus, desktop)
install(TARGETS ${APP_SIGNER_BIN} gnostr-signer-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# GSettings schema
install(FILES data/org.gnostr.Signer.gschema.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/glib-2.0/schemas)
# DBus service (session)
install(FILES data/org.nostr.Signer.service DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/services)
# Systemd user unit (daemon)
install(FILES daemon/packaging/systemd/user/gnostr-signer-daemon.service
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/systemd/user)
# Desktop file
install(FILES packaging/appimage/gnostr-signer.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# Icons (hicolor theme). Place files under data/icons/hicolor/<size>/apps/
# Expected names: org.gnostr.Signer.png for raster, org.gnostr.Signer.svg for scalable
set(ICON_BASENAME "${APP_SIGNER_FLATPAK_ID}")
set(ICON_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor)
set(ICON_INSTALL_ROOT ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor)
foreach(sz IN ITEMS 16 24 32 48 64 128 256 512)
  install(FILES ${ICON_ROOT}/${sz}x${sz}/apps/${ICON_BASENAME}.png
          DESTINATION ${ICON_INSTALL_ROOT}/${sz}x${sz}/apps
          OPTIONAL)
endforeach()
install(FILES ${ICON_ROOT}/scalable/apps/${ICON_BASENAME}.svg
        DESTINATION ${ICON_INSTALL_ROOT}/scalable/apps
        OPTIONAL)

# Update icon cache on install if available (optional)
find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)
if (GTK_UPDATE_ICON_CACHE)
  add_custom_target(update-icon-cache ALL
    COMMAND ${GTK_UPDATE_ICON_CACHE} -f -t ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor || true
    COMMENT "Updating icon cache"
  )
endif()
