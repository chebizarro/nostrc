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

file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c src/engine/*.c)
if (NOT APP_SRC)
  set(APP_SRC src/main_app.c)
endif()
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
