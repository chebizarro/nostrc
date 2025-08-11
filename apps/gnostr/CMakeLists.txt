cmake_minimum_required(VERSION 3.10)
project(gnostr)

include(GNUInstallDirs)

set(APP_CLIENT_NAME "GNostr")
set(APP_CLIENT_BIN "gnostr")
set(APP_CLIENT_FLATPAK_ID "org.gnostr.Client")
set(APP_CLIENT_GSETTINGS "org.gnostr.Client")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)

# Compile Gtk resources
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml)
set(GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND glib-compile-resources
          --target=${GRESOURCE_C}
          --generate-source
          ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML}
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/gnostr-main-window.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-timeline-view.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-composer.ui
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
  VERBATIM)

file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c src/engine/*.c src/ipc/*.c)
list(APPEND APP_SRC ${GRESOURCE_C})
if (NOT APP_SRC)
  set(APP_SRC src/main_app.c)
endif()

# Generate D-Bus proxy from canonical XML (no inline XML)
set(SIGNER_XML ${CMAKE_CURRENT_SOURCE_DIR}/../gnostr-signer/data/dbus/org.nostr.Signer.xml)
set(SIGNER_PROXY_C ${CMAKE_CURRENT_BINARY_DIR}/signer_proxy.c)
set(SIGNER_PROXY_H ${CMAKE_CURRENT_BINARY_DIR}/signer_proxy.h)
add_custom_command(
  OUTPUT ${SIGNER_PROXY_C} ${SIGNER_PROXY_H}
  COMMAND gdbus-codegen
          --generate-c-code ${CMAKE_CURRENT_BINARY_DIR}/signer_proxy
          --c-namespace Nostr
          ${SIGNER_XML}
  DEPENDS ${SIGNER_XML}
  VERBATIM)

list(APPEND APP_SRC ${SIGNER_PROXY_C})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_executable(${APP_CLIENT_BIN} ${APP_SRC})
target_compile_features(${APP_CLIENT_BIN} PRIVATE c_std_11)
target_compile_definitions(${APP_CLIENT_BIN} PRIVATE APP_ID="${APP_CLIENT_FLATPAK_ID}" APP_NAME="${APP_CLIENT_NAME}")
# Link libnostr GLib if available; fall back to core libnostr where needed
if (TARGET nostr)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE libnostr nostr_json)
endif()
target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::GTK4 PkgConfig::GLIB)
if (SQLITE3_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::SQLITE3)
endif()

install(TARGETS ${APP_CLIENT_BIN} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
