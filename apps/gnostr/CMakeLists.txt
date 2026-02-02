cmake_minimum_required(VERSION 3.30)
project(gnostr)

include(GNUInstallDirs)

set(APP_CLIENT_NAME "GNostr")
set(APP_CLIENT_BIN "gnostr")
set(APP_CLIENT_FLATPAK_ID "org.gnostr.gnostr")
set(APP_CLIENT_GSETTINGS "org.gnostr.gnostr")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)
pkg_check_modules(SOUP3 QUIET IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(QRENCODE QUIET IMPORTED_TARGET libqrencode)
pkg_check_modules(LIBPEAS REQUIRED IMPORTED_TARGET libpeas-2)

# Find blueprint-compiler for compiling .blp files to .ui files
find_program(BLUEPRINT_COMPILER blueprint-compiler)
if(BLUEPRINT_COMPILER)
  message(STATUS "Found blueprint-compiler: ${BLUEPRINT_COMPILER}")
else()
  message(WARNING "blueprint-compiler not found - using pre-compiled .ui files")
endif()

# Blueprint source files (.blp) - these are the source of truth for UI definitions
set(BLUEPRINT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/gnostr-main-window.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-session-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-timeline-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-composer.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-timeline-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/note-card-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-profile-pane.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-thread-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-note-embed.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/page-discover.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-profile-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-search-results-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-dm-inbox-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-dm-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-notifications-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-notification-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-article-card.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-articles-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-live-card.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-zap-goal-widget.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-channel-list-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-channel-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-chat-room-view.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-chat-message-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-apps-page.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-plugin-manager-panel.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-plugin-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-channel-create-dialog.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-settings-dialog.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-relay-manager.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-profile-edit.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-nwc-connect.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-zap-dialog.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-mute-list.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-login.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-about-dialog.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-report-dialog.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-app-handler-dialog.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-relay-discovery.blp
)

# Compile Blueprint files to UI files if blueprint-compiler is available
set(COMPILED_UI_FILES)
if(BLUEPRINT_COMPILER)
  foreach(BLP_FILE ${BLUEPRINT_SOURCES})
    # Get relative path and convert .blp to .ui
    file(RELATIVE_PATH BLP_REL ${CMAKE_CURRENT_SOURCE_DIR}/data ${BLP_FILE})
    string(REGEX REPLACE "\\.blp$" ".ui" UI_REL ${BLP_REL})
    set(UI_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data/${UI_REL})
    
    add_custom_command(
      OUTPUT ${UI_FILE}
      COMMAND ${BLUEPRINT_COMPILER} compile --output ${UI_FILE} ${BLP_FILE}
      DEPENDS ${BLP_FILE}
      COMMENT "Compiling Blueprint: ${BLP_REL}"
      VERBATIM
    )
    list(APPEND COMPILED_UI_FILES ${UI_FILE})
  endforeach()
  
  # Create a target for all Blueprint compilations
  add_custom_target(gnostr_blueprints ALL DEPENDS ${COMPILED_UI_FILES})
endif()

# UI files that are compiled from Blueprint sources
set(UI_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/gnostr-main-window.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-session-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-timeline-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-composer.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-timeline-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/note-card-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-profile-pane.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-thread-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-note-embed.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/page-discover.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-profile-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-search-results-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-dm-inbox-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-dm-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-notifications-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-notification-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-article-card.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-articles-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-live-card.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-zap-goal-widget.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-channel-list-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-community-list-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-channel-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-chat-room-view.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-chat-message-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-apps-page.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-plugin-manager-panel.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/widgets/gnostr-plugin-row.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-channel-create-dialog.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-settings-dialog.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-relay-manager.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-profile-edit.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-nwc-connect.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-zap-dialog.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-mute-list.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-login.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-about-dialog.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-report-dialog.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-app-handler-dialog.ui
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/dialogs/gnostr-relay-discovery.ui
)

# Compile Gtk resources
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml)
set(GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND glib-compile-resources
          --target=${GRESOURCE_C}
          --generate-source
          ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML} ${UI_FILES}
          ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/styles/gnostr.css
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
  VERBATIM)

file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c src/engine/*.c src/ipc/*.c src/util/*.c src/model/*.c src/notifications/*.c)
# Exclude CLI entrypoint from the GUI app target to avoid duplicate main()
list(REMOVE_ITEM APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/cli_live_logger.c)
# Platform-specific files: on macOS, exclude the Linux C implementations
# (the macOS Objective-C implementations are added later via target_sources)
if (APPLE)
  list(REMOVE_ITEM APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gnostr-tray-icon.c)
  list(REMOVE_ITEM APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/notifications/desktop_notify.c)
endif()
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
# Plugin search path for development builds - points to build/plugins directory
target_compile_definitions(${APP_CLIENT_BIN} PRIVATE GNOSTR_DEV_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")
# Find secp256k1 for key derivation in identity.c
pkg_check_modules(SECP256K1 QUIET libsecp256k1)
# Project root for finding shared libraries (works whether built standalone or from top-level)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Generate nostr-config.h if not already present (for standalone builds or first-time builds)
# This header is normally generated by libnostr's CMake, but we need it available immediately
set(_NOSTR_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_NOSTR_CONFIG_H "${_NOSTR_CONFIG_DIR}/nostr-config.h")
if(NOT EXISTS "${_NOSTR_CONFIG_H}")
  file(MAKE_DIRECTORY "${_NOSTR_CONFIG_DIR}")
  file(WRITE "${_NOSTR_CONFIG_H}" "/* Generated by gnostr CMake for standalone build */
#define NOSTR_HAVE_GLIB 1
#define NOSTR_HAVE_LIBGO 1
")
endif()

# Include GI wrapper and core headers for SimplePool and nostr models
target_include_directories(${APP_CLIENT_BIN} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/util
  ${PROJECT_ROOT}/gobject/include
  ${PROJECT_ROOT}/libnostr/include
  ${_NOSTR_CONFIG_DIR}
  ${PROJECT_ROOT}/libgo/include
  ${PROJECT_ROOT}/nips/nip46/include
  ${PROJECT_ROOT}/nips/nip47/include
  ${PROJECT_ROOT}/nips/nip19/include
  ${PROJECT_ROOT}/nips/nip10/include
  ${PROJECT_ROOT}/nips/nip17/include
  ${PROJECT_ROOT}/nips/nip25/include
  ${SECP256K1_INCLUDE_DIRS})
# Compile the GI wrapper implementation into the app (no separate gobject lib target)
# NOTE: Goroutine code is now consolidated into nostr_simple_pool.c
target_sources(${APP_CLIENT_BIN} PRIVATE
  ${PROJECT_ROOT}/gobject/src/nostr_simple_pool.c
  ${PROJECT_ROOT}/gobject/src/nostr_relay.c)
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
if (TARGET nip25)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nip25)
endif()
if (TARGET nostr_nip46_core)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostr_nip46_core)
endif()
if (TARGET nostr_nip17_core)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostr_nip17_core)
endif()
if (TARGET nostr_nip47_core)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostr_nip47_core)
endif()
target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::ADWAITA PkgConfig::JSON_GLIB)
# Link secp256k1 for identity/key derivation
if (SECP256K1_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE ${SECP256K1_LIBRARIES})
endif()
# Link math library on Linux
if (UNIX AND NOT APPLE)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE m)
endif()
if (SOUP3_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::SOUP3)
  target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_SOUP3=1)
endif()
if (SQLITE3_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::SQLITE3)
endif()
if (QRENCODE_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::QRENCODE)
  target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_QRENCODE=1)
endif()

# ---- libpeas for plugin system (required) ----
target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::LIBPEAS)
target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_LIBPEAS=1)

# ---- Bundled plugins ----
# nip47-nwc disabled pending nostrc-qdqf (gnostr_plugin_context API)
# add_subdirectory(plugins/nip47-nwc)
add_subdirectory(plugins/nip98-httpauth)
add_subdirectory(plugins/nip46-connect)
add_subdirectory(plugins/nip17-dms)
add_subdirectory(plugins/nip55-androidsigner)
add_subdirectory(plugins/nip49-keyencrypt)
add_subdirectory(plugins/nip77-negentropy)
add_subdirectory(plugins/nip57-zaps)
add_subdirectory(plugins/nip34-git)

# Make gnostr depend on plugins so they're built together
add_dependencies(${APP_CLIENT_BIN}
  nip98-httpauth
  nip46-connect
  nip17-dms
  nip55-androidsigner
  nip49-keyencrypt
  nip77-negentropy
  nip57-zaps
  nip34-git
)

# ---- OpenSSL for DM file encryption (AES-GCM) ----
# OpenSSL is already found at the top-level CMakeLists.txt
if (OPENSSL_FOUND)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE OpenSSL::Crypto)
  target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_OPENSSL=1)
endif()

# ---- Secure Key Storage (platform-native) ----
# Linux: libsecret (GNOME Keyring / KDE Wallet)
# macOS: Security.framework (Keychain Services)
if (APPLE)
  # macOS Keychain - Security framework is always available
  target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_MACOS_KEYCHAIN=1)
  # Security framework already linked for nostrdb, but ensure it's available
  find_library(SECURITY_FRAMEWORK Security REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  message(STATUS "Secure key storage: macOS Keychain (Security.framework)")
else()
  # Linux: try to find libsecret
  pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
  if (LIBSECRET_FOUND)
    target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::LIBSECRET)
    target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_LIBSECRET=1)
    message(STATUS "Secure key storage: libsecret (GNOME Keyring / KDE Wallet)")
  else()
    message(STATUS "Secure key storage: NOT AVAILABLE (install libsecret-1-dev)")
  endif()
endif()

# ---- System Tray / Menu Bar Icon ----
# Linux: Uses pure D-Bus StatusNotifierItem protocol (GTK4 compatible)
#        with optional libdbusmenu-glib for menu support
#        This avoids libayatana-appindicator3 which requires GTK3 headers
# macOS: Uses NSStatusItem via Objective-C wrapper (AppKit)
if (APPLE)
  # macOS: Add the Objective-C menu bar and notification implementations
  # (Linux C implementations were already excluded from APP_SRC before add_executable)
  target_sources(${APP_CLIENT_BIN} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gnostr-menubar-macos.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/notifications/desktop_notify_macos.m)

  # Enable ARC (Automatic Reference Counting) for Objective-C files
  set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gnostr-menubar-macos.m
    ${CMAKE_CURRENT_SOURCE_DIR}/src/notifications/desktop_notify_macos.m
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")

  # Link AppKit framework for NSStatusItem
  find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE ${APPKIT_FRAMEWORK})

  # Link UserNotifications framework for UNUserNotificationCenter (macOS 10.14+)
  find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE ${USERNOTIFICATIONS_FRAMEWORK})

  message(STATUS "Menu bar: macOS NSStatusItem (AppKit)")
  message(STATUS "Desktop notifications: macOS UNUserNotificationCenter")
elseif (UNIX)
  # Linux: Use pure D-Bus StatusNotifierItem (GTK4 compatible, no GTK3 headers)
  # libdbusmenu-glib is optional but provides right-click menu support
  pkg_check_modules(DBUSMENU_GLIB QUIET IMPORTED_TARGET dbusmenu-glib-0.4)
  if (DBUSMENU_GLIB_FOUND)
    target_link_libraries(${APP_CLIENT_BIN} PRIVATE PkgConfig::DBUSMENU_GLIB)
    target_compile_definitions(${APP_CLIENT_BIN} PRIVATE HAVE_DBUSMENU=1)
    message(STATUS "System tray: StatusNotifierItem (D-Bus) + dbusmenu-glib")
  else()
    message(STATUS "System tray: StatusNotifierItem (D-Bus) - no menu support (install libdbusmenu-glib-dev for menus)")
  endif()
endif()

# Link nostrdb_storage for local database support
set(_NDB_VENDOR_DIR ${PROJECT_ROOT}/third_party/nostrdb)
if (TARGET nostrdb_storage)
  target_link_libraries(${APP_CLIENT_BIN} PRIVATE nostrdb_storage)
  # Add nostrdb include paths for direct API access (subscriptions, etc.)
  target_include_directories(${APP_CLIENT_BIN} PRIVATE
    ${_NDB_VENDOR_DIR}/src
    ${_NDB_VENDOR_DIR}/ccan
    ${_NDB_VENDOR_DIR}/deps/flatcc/include)
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
  ${PROJECT_ROOT}/gobject/include
  ${PROJECT_ROOT}/libnostr/include
  ${_NOSTR_CONFIG_DIR}
  ${PROJECT_ROOT}/libgo/include
  ${PROJECT_ROOT}/nips/nip46/include
  ${_NDB_VENDOR_DIR}/src
  ${_NDB_VENDOR_DIR}/ccan
  ${_NDB_VENDOR_DIR}/deps/flatcc/include)
# Compile GI wrapper into the CLI as well
# NOTE: Goroutine code is now consolidated into nostr_simple_pool.c
target_sources(gnostr-live-log PRIVATE
  ${PROJECT_ROOT}/gobject/src/nostr_simple_pool.c
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
if (TARGET nostrdb_storage)
  target_link_libraries(gnostr-live-log PRIVATE nostrdb_storage)
  if (NOT TARGET nostrdb)
    if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
      if (APPLE)
        target_link_options(gnostr-live-log PRIVATE -Wl,-force_load,${_NDB_VENDOR_DIR}/libnostrdb.a)
        if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          target_link_libraries(gnostr-live-log PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
        endif()
        target_link_libraries(gnostr-live-log PRIVATE "-framework Security")
      else()
        target_link_options(gnostr-live-log PRIVATE -Wl,--whole-archive ${_NDB_VENDOR_DIR}/libnostrdb.a -Wl,--no-whole-archive)
        if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
          target_link_libraries(gnostr-live-log PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
        endif()
      endif()
    endif()
  endif()
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
  target_include_directories(gnostr-test-relays PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/libnostr/include
    ${_NOSTR_CONFIG_DIR})
  target_sources(gnostr-test-relays PRIVATE src/util/relays.c)
  # Disable NIP-65 async functionality for this test (avoids nostr_simple_pool dependency)
  target_compile_definitions(gnostr-test-relays PRIVATE GNOSTR_RELAY_TEST_ONLY=1)
  target_link_libraries(gnostr-test-relays PRIVATE PkgConfig::GLIB PkgConfig::GIO PkgConfig::JSON_GLIB)
  _gnostr_apply_sanitizers(gnostr-test-relays)
  add_test(NAME gnostr-test-relays COMMAND gnostr-test-relays)

  # Blossom (BUD-01/BUD-02) unit tests
  add_executable(gnostr-test-blossom tests/test_blossom.c)
  target_compile_features(gnostr-test-blossom PRIVATE c_std_11)
  target_include_directories(gnostr-test-blossom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROJECT_ROOT}/libnostr/include
    ${_NOSTR_CONFIG_DIR}
    ${PROJECT_ROOT}/nips/nip19/include
    ${PROJECT_ROOT}/nips/nip46/include
    ${PROJECT_ROOT}/libgo/include
    ${PROJECT_ROOT}/gobject/include)
  target_sources(gnostr-test-blossom PRIVATE
    src/util/blossom.c
    src/util/blossom_settings.c
    src/util/relays.c
    src/util/relay_info.c
    src/ipc/signer_ipc.c
    src/ipc/gnostr-signer-service.c
    ${SIGNER_PROXY_C}
    ${PROJECT_ROOT}/gobject/src/nostr_simple_pool.c
    ${PROJECT_ROOT}/gobject/src/nostr_relay.c)
  target_link_libraries(gnostr-test-blossom PRIVATE PkgConfig::GLIB PkgConfig::GIO PkgConfig::JSON_GLIB PkgConfig::SOUP3 nostr nip19 libgo)
  if (TARGET nostr_nip46_core)
    target_link_libraries(gnostr-test-blossom PRIVATE nostr_nip46_core)
  endif()
  _gnostr_apply_sanitizers(gnostr-test-blossom)
  add_test(NAME gnostr-test-blossom COMMAND gnostr-test-blossom)
  set_tests_properties(gnostr-test-blossom PROPERTIES
    TIMEOUT 30
    LABELS "blossom;unit;gnostr")

  # Blossom settings (kind 10063) unit tests
  add_executable(gnostr-test-blossom-settings tests/test_blossom_settings.c)
  target_compile_features(gnostr-test-blossom-settings PRIVATE c_std_11)
  target_include_directories(gnostr-test-blossom-settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROJECT_ROOT}/libnostr/include
    ${_NOSTR_CONFIG_DIR}
    ${PROJECT_ROOT}/nips/nip46/include
    ${PROJECT_ROOT}/libgo/include
    ${PROJECT_ROOT}/gobject/include)
  target_sources(gnostr-test-blossom-settings PRIVATE
    src/util/blossom_settings.c
    src/util/relays.c
    src/util/relay_info.c
    src/ipc/signer_ipc.c
    src/ipc/gnostr-signer-service.c
    ${SIGNER_PROXY_C}
    ${PROJECT_ROOT}/gobject/src/nostr_simple_pool.c
    ${PROJECT_ROOT}/gobject/src/nostr_relay.c)
  target_link_libraries(gnostr-test-blossom-settings PRIVATE
    PkgConfig::GLIB PkgConfig::GIO PkgConfig::JSON_GLIB PkgConfig::SOUP3
    nostr libgo)
  if (TARGET nostr_nip46_core)
    target_link_libraries(gnostr-test-blossom-settings PRIVATE nostr_nip46_core)
  endif()
  _gnostr_apply_sanitizers(gnostr-test-blossom-settings)
  add_test(NAME gnostr-test-blossom-settings COMMAND gnostr-test-blossom-settings)
  set_tests_properties(gnostr-test-blossom-settings PROPERTIES
    TIMEOUT 30
    LABELS "blossom;settings;unit;gnostr"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}")

  # Secure keystore unit tests
  add_executable(gnostr-test-keystore tests/test_keystore.c)
  target_compile_features(gnostr-test-keystore PRIVATE c_std_11)
  target_include_directories(gnostr-test-keystore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # Include the appropriate keystore implementation based on platform
  if (APPLE)
    target_sources(gnostr-test-keystore PRIVATE
      src/util/keystore_macos.c
      src/util/keystore_stub.c)
    target_compile_definitions(gnostr-test-keystore PRIVATE HAVE_MACOS_KEYCHAIN=1)
    target_link_libraries(gnostr-test-keystore PRIVATE ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  elseif (LIBSECRET_FOUND)
    target_sources(gnostr-test-keystore PRIVATE
      src/util/keystore_libsecret.c
      src/util/keystore_stub.c)
    target_compile_definitions(gnostr-test-keystore PRIVATE HAVE_LIBSECRET=1)
    target_link_libraries(gnostr-test-keystore PRIVATE PkgConfig::LIBSECRET)
  else()
    target_sources(gnostr-test-keystore PRIVATE src/util/keystore_stub.c)
  endif()
  target_link_libraries(gnostr-test-keystore PRIVATE PkgConfig::GLIB PkgConfig::GIO)
  _gnostr_apply_sanitizers(gnostr-test-keystore)
  add_test(NAME gnostr-test-keystore COMMAND gnostr-test-keystore)
  set_tests_properties(gnostr-test-keystore PROPERTIES
    TIMEOUT 30
    LABELS "keystore;security;unit;gnostr")

  # NIP-46 E2E tests
  add_executable(gnostr-test-nip46-e2e tests/test_nip46_e2e.c)
  target_compile_features(gnostr-test-nip46-e2e PRIVATE c_std_11)
  target_include_directories(gnostr-test-nip46-e2e PRIVATE
    ${PROJECT_ROOT}/nips/nip46/include
    ${PROJECT_ROOT}/nips/nip04/include
    ${PROJECT_ROOT}/libnostr/include
    ${_NOSTR_CONFIG_DIR}
    ${PROJECT_ROOT}/libjson/include)
  if (TARGET nostr_nip46_core)
    target_link_libraries(gnostr-test-nip46-e2e PRIVATE nostr_nip46_core)
  endif()
  if (TARGET libnostr)
    target_link_libraries(gnostr-test-nip46-e2e PRIVATE libnostr)
  endif()
  if (TARGET nostr_json)
    target_link_libraries(gnostr-test-nip46-e2e PRIVATE nostr_json)
  endif()
  if (TARGET nip04)
    target_link_libraries(gnostr-test-nip46-e2e PRIVATE nip04)
  endif()
  _gnostr_apply_sanitizers(gnostr-test-nip46-e2e)
  add_test(NAME gnostr-test-nip46-e2e COMMAND gnostr-test-nip46-e2e)
  set_tests_properties(gnostr-test-nip46-e2e PROPERTIES
    TIMEOUT 60
    LABELS "nip46;e2e;gnostr")

  # Thread graph unit tests (nostrc-v5d)
  add_executable(gnostr-test-thread-graph tests/test_thread_graph.c)
  target_compile_features(gnostr-test-thread-graph PRIVATE c_std_11)
  target_link_libraries(gnostr-test-thread-graph PRIVATE PkgConfig::GLIB)
  _gnostr_apply_sanitizers(gnostr-test-thread-graph)
  add_test(NAME gnostr-test-thread-graph COMMAND gnostr-test-thread-graph)
  set_tests_properties(gnostr-test-thread-graph PROPERTIES
    TIMEOUT 30
    LABELS "thread;graph;unit;gnostr")

  # Timeline query unit tests (nostrc-whu3)
  add_executable(gnostr-test-timeline-query tests/test_timeline_query.c src/model/gn-timeline-query.c)
  target_compile_features(gnostr-test-timeline-query PRIVATE c_std_11)
  target_include_directories(gnostr-test-timeline-query PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(gnostr-test-timeline-query PRIVATE PkgConfig::GLIB)
  _gnostr_apply_sanitizers(gnostr-test-timeline-query)
  add_test(NAME gnostr-test-timeline-query COMMAND gnostr-test-timeline-query)
  set_tests_properties(gnostr-test-timeline-query PROPERTIES
    TIMEOUT 30
    LABELS "timeline;query;unit;gnostr")

  # NIP-66 Relay Discovery unit tests
  add_executable(gnostr-test-nip66-relay-discovery tests/test_nip66_relay_discovery.c)
  target_compile_features(gnostr-test-nip66-relay-discovery PRIVATE c_std_11)
  target_include_directories(gnostr-test-nip66-relay-discovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROJECT_ROOT}/libnostr/include
    ${PROJECT_ROOT}/libjson/include
    ${PROJECT_ROOT}/libgo/include
    ${_NOSTR_CONFIG_DIR})
  target_sources(gnostr-test-nip66-relay-discovery PRIVATE src/util/nip66_relay_discovery.c)
  # Disable async discovery (avoids nostr_simple_pool and relay dependencies)
  target_compile_definitions(gnostr-test-nip66-relay-discovery PRIVATE GNOSTR_NIP66_TEST_ONLY=1)
  target_link_libraries(gnostr-test-nip66-relay-discovery PRIVATE PkgConfig::GLIB PkgConfig::GIO PkgConfig::JSON_GLIB nostr_json libgo)
  _gnostr_apply_sanitizers(gnostr-test-nip66-relay-discovery)
  add_test(NAME gnostr-test-nip66-relay-discovery COMMAND gnostr-test-nip66-relay-discovery)
  set_tests_properties(gnostr-test-nip66-relay-discovery PROPERTIES
    TIMEOUT 30
    LABELS "nip66;relay;discovery;unit;gnostr")

endif()

# ---- GSettings schema compilation (development builds) ----
# Compile schemas into the build directory so the app can run without system-wide install
set(GNOSTR_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/schemas)
set(GNOSTR_SCHEMA_FILE ${GNOSTR_SCHEMA_DIR}/org.gnostr.gnostr.gschema.xml)
set(GNOSTR_SCHEMA_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled)

find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS)
  add_custom_command(
    OUTPUT ${GNOSTR_SCHEMA_OUTPUT}
    COMMAND ${GLIB_COMPILE_SCHEMAS}
            ${GNOSTR_SCHEMA_DIR}
            --targetdir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${GNOSTR_SCHEMA_FILE}
    COMMENT "Compiling GSettings schemas for gnostr"
    VERBATIM
  )
  add_custom_target(gnostr-schemas ALL DEPENDS ${GNOSTR_SCHEMA_OUTPUT})
  add_dependencies(${APP_CLIENT_BIN} gnostr-schemas)
else()
  message(WARNING "glib-compile-schemas not found - GSettings schemas will not be compiled")
endif()

# ---- GSettings schema install ----
install(FILES data/schemas/org.gnostr.gnostr.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)

# ---- Desktop entry ----
install(FILES data/org.gnostr.gnostr.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

# ---- AppStream metadata ----
install(FILES data/org.gnostr.gnostr.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)

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
