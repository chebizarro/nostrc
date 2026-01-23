cmake_minimum_required(VERSION 3.30)
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
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)

# Optional libsodium for secure memory operations
pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
if (SODIUM_FOUND)
  message(STATUS "libsodium found - secure memory will use sodium APIs")
else()
  message(STATUS "libsodium not found - secure memory will use system fallbacks")
endif()

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
# NIP-46 core headers for bunker service
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip46/include)
# NIP-19 for bech32 encoding
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip19/include)
# NIP-49 for encrypted key backup (ncryptsec)
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip49/include)
# NIP-06 for mnemonic derivation
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip06/include)
# libnostr core for keys.h and crypto/bip39.h
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/libnostr/include)
# Link against core to resolve owner APIs
target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::GTK4 PkgConfig::GLIB PkgConfig::ADWAITA PkgConfig::JSON_GLIB nostr_nip55l_core nip04)
# Link NIP-46 core for bunker service
if (TARGET nostr_nip46_core)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nostr_nip46_core)
endif()
# Link NIP-19 for bech32 encoding
if (TARGET nip19)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nip19)
endif()
# Link NIP-49 for encrypted key backup
if (TARGET nostr_nip49_glib)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nostr_nip49_glib nostr_nip49_core)
elseif (TARGET nostr_nip49_core)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nostr_nip49_core)
endif()
# Link NIP-06 for mnemonic derivation
if (TARGET nip06)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nip06)
endif()
# Link nostr core for BIP-39 and keys
if (TARGET nostr)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nostr)
endif()

# Include project root so we can include gnome/seahorse/secret_store.h from the app
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR})

# If Seahorse helper lib is built, link it to enable secret deletion helpers
if (TARGET gnostr-secret)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE gnostr-secret)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_LIBSECRET=1)
endif()

# Link libsecret directly for secret-storage.c when available
if (LIBSECRET_FOUND AND NOT TARGET gnostr-secret)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::LIBSECRET)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_LIBSECRET=1)
  message(STATUS "gnostr-signer: libsecret linked directly for secure key storage")
elseif(APPLE)
  # macOS: Link Security.framework for Keychain support
  find_library(SECURITY_FRAMEWORK Security REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  message(STATUS "gnostr-signer: Security.framework linked for Keychain support")
endif()

# Link libsodium for secure memory if available
if (SODIUM_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::SODIUM)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_SODIUM=1)
endif()

# Do not enable GNOSTR_HAVE_LIBSECRET unless helper lib is built; otherwise
# we'd reference symbols not linked into the app.

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
target_link_libraries(gnostr-signer-daemon PRIVATE nostr_nip55l_glib nostr_nip5f_core nip04 PkgConfig::GLIB ${NSYNC_LIB})

# Link libsodium for daemon secure memory if available
if (SODIUM_FOUND)
  target_link_libraries(gnostr-signer-daemon PRIVATE PkgConfig::SODIUM)
  target_compile_definitions(gnostr-signer-daemon PRIVATE GNOSTR_HAVE_SODIUM=1)
endif()

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

# ==============================================================================
# Unit Tests
# ==============================================================================
# Include test suite (uses CTest/GLib testing framework)
option(BUILD_SIGNER_TESTS "Build gnostr-signer unit tests" ON)
if (BUILD_SIGNER_TESTS)
  add_subdirectory(tests)
endif()

# ==============================================================================
# macOS Application Bundle Support
# ==============================================================================
if(APPLE)
  option(BUILD_MACOS_BUNDLE "Build macOS application bundle" OFF)

  if(BUILD_MACOS_BUNDLE)
    # Application version for bundle
    set(GNOSTR_SIGNER_VERSION "1.0.0" CACHE STRING "Application version for macOS bundle")

    # Path to packaging files
    set(MACOS_PACKAGING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos)

    # Generate Info.plist from template
    configure_file(
      ${MACOS_PACKAGING_DIR}/Info.plist.in
      ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
      @ONLY
    )
    string(REPLACE "@VERSION@" "${GNOSTR_SIGNER_VERSION}" _info_plist_content "")

    # Configure the Info.plist with version substitution
    file(READ ${MACOS_PACKAGING_DIR}/Info.plist.in INFO_PLIST_CONTENT)
    string(REPLACE "@VERSION@" "${GNOSTR_SIGNER_VERSION}" INFO_PLIST_CONTENT "${INFO_PLIST_CONTENT}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/Info.plist "${INFO_PLIST_CONTENT}")

    # Custom target to build macOS .app bundle
    add_custom_target(macos-bundle
      COMMAND ${CMAKE_COMMAND} -E echo "Building macOS application bundle..."
      COMMAND ${MACOS_PACKAGING_DIR}/create-dmg.sh
        --build-dir "${CMAKE_BINARY_DIR}"
        --version "${GNOSTR_SIGNER_VERSION}"
        --output-dir "${CMAKE_BINARY_DIR}/bundle"
        --skip-dmg
      DEPENDS ${APP_SIGNER_BIN} gnostr-signer-daemon
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Creating macOS application bundle"
      VERBATIM
    )

    # Custom target to build DMG installer
    add_custom_target(macos-dmg
      COMMAND ${CMAKE_COMMAND} -E echo "Building macOS DMG installer..."
      COMMAND ${MACOS_PACKAGING_DIR}/create-dmg.sh
        --build-dir "${CMAKE_BINARY_DIR}"
        --version "${GNOSTR_SIGNER_VERSION}"
        --output-dir "${CMAKE_BINARY_DIR}/bundle"
      DEPENDS ${APP_SIGNER_BIN} gnostr-signer-daemon
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Creating macOS DMG installer"
      VERBATIM
    )

    # Custom target for signed release DMG (requires DEVELOPER_ID env var)
    add_custom_target(macos-dmg-signed
      COMMAND ${CMAKE_COMMAND} -E echo "Building signed macOS DMG installer..."
      COMMAND ${MACOS_PACKAGING_DIR}/create-dmg.sh
        --build-dir "${CMAKE_BINARY_DIR}"
        --version "${GNOSTR_SIGNER_VERSION}"
        --output-dir "${CMAKE_BINARY_DIR}/bundle"
        --sign "$ENV{DEVELOPER_ID}"
      DEPENDS ${APP_SIGNER_BIN} gnostr-signer-daemon
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Creating signed macOS DMG installer"
      VERBATIM
    )

    # Custom target for notarized release DMG (requires Apple credentials)
    add_custom_target(macos-dmg-notarized
      COMMAND ${CMAKE_COMMAND} -E echo "Building notarized macOS DMG installer..."
      COMMAND ${MACOS_PACKAGING_DIR}/create-dmg.sh
        --build-dir "${CMAKE_BINARY_DIR}"
        --version "${GNOSTR_SIGNER_VERSION}"
        --output-dir "${CMAKE_BINARY_DIR}/bundle"
        --sign "$ENV{DEVELOPER_ID}"
        --notarize
      DEPENDS ${APP_SIGNER_BIN} gnostr-signer-daemon
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Creating notarized macOS DMG installer"
      VERBATIM
    )

    message(STATUS "macOS bundle support enabled. Available targets:")
    message(STATUS "  - macos-bundle: Build .app bundle only")
    message(STATUS "  - macos-dmg: Build unsigned DMG installer")
    message(STATUS "  - macos-dmg-signed: Build signed DMG (requires DEVELOPER_ID env var)")
    message(STATUS "  - macos-dmg-notarized: Build notarized DMG (requires Apple credentials)")
  endif()
endif()
