cmake_minimum_required(VERSION 3.30)
project(gnostr)

include(GNUInstallDirs)

set(APP_CLIENT_NAME "GNostr")
set(APP_CLIENT_BIN "gnostr")
set(APP_CLIENT_FLATPAK_ID "org.gnostr.Client")
set(APP_CLIENT_GSETTINGS "org.gnostr.Client")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)
pkg_check_modules(SOUP3 QUIET IMPORTED_TARGET libsoup-3.0)

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
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-timeline-row.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/note-card-row.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-profile-pane.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-settings-dialog.ui
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-relay-manager.ui
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
  VERBATIM)

file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c src/engine/*.c src/ipc/*.c src/util/*.c src/model/*.c)
# Exclude CLI entrypoint from the GUI app target to avoid duplicate main()
list(REMOVE_ITEM APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/cli_live_logger.c)
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
# Enable real SimplePool-backed timeline fetching in the UI
target_compile_definitions(${APP_CLIENT_BIN} PRIVATE GNOSTR_ENABLE_REAL_SIMPLEPOOL)
# Include GI wrapper and core headers for SimplePool and nostr models
target_include_directories(${APP_CLIENT_BIN} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/util
  ${CMAKE_SOURCE_DIR}/gobject/include
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/libgo/include
  ${CMAKE_SOURCE_DIR}/nips/nip46/include
  ${CMAKE_SOURCE_DIR}/nips/nip19/include
  ${CMAKE_SOURCE_DIR}/nips/nip10/include)
# Compile the GI wrapper implementation into the app (no separate gobject lib target)
# NOTE: Goroutine code is now consolidated into nostr_simple_pool.c
target_sources(${APP_CLIENT_BIN} PRIVATE
  ${CMAKE_SOURCE_DIR}/gobject/src/nostr_simple_pool.c
  ${CMAKE_SOURCE_DIR}/gobject/src/nostr_relay.c)
# Link JSON shim which (privately) links libnostr; avoid linking libnostr twice to prevent ODR/ASan issues
if (TARGET nostr_json)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostr_json)
endif()
if (TARGET nip19)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nip19)
endif()
if (TARGET nip10)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nip10)
endif()
if (TARGET nostr_nip46_core)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostr_nip46_core)
endif()
target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::JSON_GLIB)
if (SOUP3_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::SOUP3)
  target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_SOUP3=1)
endif()
if (SQLITE3_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::SQLITE3)
endif()

# Link nostrdb_storage for local database support
set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
if (TARGET nostrdb_storage)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostrdb_storage)
  if (NOT TARGET nostrdb)
    if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
      if (APPLE)
        target_link_options(${APP_CLIENT_BIN} PRIVATE -Wl,-force_load,$<TARGET_FILE:nostrdb_storage>)
        target_link_options(${APP_CLIENT_BIN} PRIVATE -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
        if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          target_link_libraries(${APP_CLIENT_BIN} PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
        endif()
        # Security framework required by nostrdb for SecRandomCopyBytes
        target_link_options(${APP_CLIENT_BIN} PRIVATE "SHELL:-framework Security")
      else()
        target_link_options(${APP_CLIENT_BIN} PRIVATE -Wl,--start-group)
        target_link_libraries(${APP_CLIENT_BIN} PRIVATE -Wl,--whole-archive $<TARGET_FILE:nostrdb_storage> ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
        if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          target_link_libraries(${APP_CLIENT_BIN} PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
        endif()
        target_link_options(${APP_CLIENT_BIN} PRIVATE -Wl,--end-group)
      endif()
    endif()
  endif()
endif()

# Local sanitizer helper (root apply_sanitizers() is defined after add_subdirectory(apps))
# Use GNOSTR_ENABLE_* toggles for the app; fallback to GO_ENABLE_* if unset.
option(GNOSTR_ENABLE_ASAN "Enable AddressSanitizer for GNostr app" OFF)
option(GNOSTR_ENABLE_UBSAN "Enable UBSan for GNostr app" OFF)
option(GNOSTR_ENABLE_TSAN "Enable TSan for GNostr app" OFF)

function(_gnostr_apply_sanitizers target)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Prefer GNOSTR_* toggles; fallback to GO_* to match root cache if the app-specific ones are OFF.
    if (GNOSTR_ENABLE_TSAN OR GO_ENABLE_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    else()
      if (GNOSTR_ENABLE_ASAN OR GO_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
      endif()
      if (GNOSTR_ENABLE_UBSAN OR GO_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
      endif()
    endif()
  endif()
endfunction()

_gnostr_apply_sanitizers(${APP_CLIENT_BIN})

# ---- Headless live logger CLI ----
add_executable(gnostr-live-log src/cli_live_logger.c)
target_compile_features(gnostr-live-log PRIVATE c_std_11)
target_compile_definitions(gnostr-live-log PRIVATE GNOSTR_ENABLE_REAL_SIMPLEPOOL)
target_include_directories(gnostr-live-log PRIVATE
  ${CMAKE_SOURCE_DIR}/gobject/include
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/libgo/include
  ${CMAKE_SOURCE_DIR}/nips/nip46/include)
# Compile GI wrapper into the CLI as well
# NOTE: Goroutine code is now consolidated into nostr_simple_pool.c
target_sources(gnostr-live-log PRIVATE
  ${CMAKE_SOURCE_DIR}/gobject/src/nostr_simple_pool.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/storage_ndb.c)
if (TARGET nostr_json)
  target_link_libraries(gnostr-live-log PRIVATE nostr_json)
endif()
if (TARGET nip19)
  target_link_libraries(gnostr-live-log PRIVATE nip19)
endif()
if (TARGET nostr_nip46_core)
  target_link_libraries(gnostr-live-log PRIVATE nostr_nip46_core)
endif()
if (TARGET libgo)
  target_link_libraries(gnostr-live-log PRIVATE libgo)
endif()
target_link_libraries(gnostr-live-log PRIVATE PkgConfig::GLIB)
if (SQLITE3_FOUND)
  target_link_libraries(gnostr-live-log PRIVATE PkgConfig::SQLITE3)
endif()
_gnostr_apply_sanitizers(gnostr-live-log)

install(TARGETS ${APP_CLIENT_BIN} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- Tests ----
include(CTest)
if (BUILD_TESTING)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
  add_executable(gnostr-test-relays tests/test_relays.c)
  target_compile_features(gnostr-test-relays PRIVATE c_std_11)
  target_include_directories(gnostr-test-relays PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_sources(gnostr-test-relays PRIVATE src/util/relays.c)
  target_link_libraries(gnostr-test-relays PRIVATE PkgConfig::GLIB PkgConfig::GIO)
  _gnostr_apply_sanitizers(gnostr-test-relays)
  add_test(NAME gnostr-test-relays COMMAND gnostr-test-relays)
endif()

# ---- GSettings schema install ----
install(FILES data/schemas/org.gnostr.gnostr.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)

# ---- Desktop entry ----
install(FILES packaging/appimage/gnostr.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# ---- Icons (hicolor) ----
set(ICON_BASENAME "${APP_CLIENT_FLATPAK_ID}")
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
