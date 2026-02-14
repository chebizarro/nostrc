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

# Optional libsodium for secure memory operations and Ed25519 support
pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
if (SODIUM_FOUND)
  message(STATUS "libsodium found - secure memory will use sodium APIs, Ed25519 enabled")
else()
  message(STATUS "libsodium not found - secure memory will use system fallbacks, Ed25519 disabled")
endif()

# Optional libsecp256k1 for direct Schnorr signature operations
pkg_check_modules(SECP256K1 QUIET IMPORTED_TARGET libsecp256k1)
if (SECP256K1_FOUND)
  message(STATUS "libsecp256k1 found - direct secp256k1 operations enabled")
else()
  message(STATUS "libsecp256k1 not found - using libnostr for secp256k1 operations")
endif()

# Optional PKCS#11 support for hardware security modules (HSM)
option(GNOSTR_SIGNER_WITH_PKCS11 "Enable PKCS#11 HSM support" ON)
if (GNOSTR_SIGNER_WITH_PKCS11)
  pkg_check_modules(P11KIT QUIET IMPORTED_TARGET p11-kit-1)
  if (P11KIT_FOUND)
    message(STATUS "p11-kit found - PKCS#11 HSM support enabled")
  else()
    message(STATUS "p11-kit not found - PKCS#11 HSM support disabled (mock provider only)")
  endif()
endif()

# Optional hidapi for hardware wallet support (Ledger, Trezor)
option(GNOSTR_SIGNER_WITH_HW_WALLET "Enable hardware wallet support (Ledger/Trezor)" ON)
if (GNOSTR_SIGNER_WITH_HW_WALLET)
  pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
  if (NOT HIDAPI_FOUND)
    # Try platform-specific variants
    if (APPLE)
      pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
    elseif(WIN32)
      pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
    else()
      # Linux: try hidapi-libusb or hidapi-hidraw
      pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi-libusb)
      if (NOT HIDAPI_FOUND)
        pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi-hidraw)
      endif()
    endif()
  endif()
  if (HIDAPI_FOUND)
    message(STATUS "hidapi found - Hardware wallet support enabled (Ledger/Trezor)")
  else()
    message(STATUS "hidapi not found - Hardware wallet support disabled")
    message(STATUS "  Install hidapi: brew install hidapi (macOS), apt install libhidapi-dev (Linux)")
  endif()
endif()

# Optional libqrencode for QR code generation
pkg_check_modules(QRENCODE QUIET IMPORTED_TARGET libqrencode)
if (QRENCODE_FOUND)
  message(STATUS "libqrencode found - QR code generation enabled")
else()
  message(STATUS "libqrencode not found - QR code generation disabled")
  message(STATUS "  Install libqrencode: brew install qrencode (macOS), apt install libqrencode-dev (Linux)")
endif()

# Optional zbar for QR code scanning
pkg_check_modules(ZBAR QUIET IMPORTED_TARGET zbar)
if (ZBAR_FOUND)
  message(STATUS "zbar found - QR code scanning enabled")
else()
  message(STATUS "zbar not found - QR code scanning disabled")
  message(STATUS "  Install zbar: brew install zbar (macOS), apt install libzbar-dev (Linux)")
endif()

# Optional GStreamer for camera-based QR scanning
pkg_check_modules(GST QUIET IMPORTED_TARGET gstreamer-1.0 gstreamer-video-1.0)
if (GST_FOUND)
  message(STATUS "GStreamer found - Camera QR scanning enabled")
else()
  message(STATUS "GStreamer not found - Camera QR scanning disabled (clipboard/file scanning still available)")
endif()

# Find blueprint-compiler for compiling .blp files to .ui files
find_program(BLUEPRINT_COMPILER blueprint-compiler)
if(BLUEPRINT_COMPILER)
  message(STATUS "Found blueprint-compiler: ${BLUEPRINT_COMPILER}")
else()
  message(WARNING "blueprint-compiler not found - using pre-compiled .ui files")
endif()

# Blueprint source files (.blp) - these are the source of truth for UI definitions
# Note: Some files that failed decompilation are kept as .ui only
file(GLOB_RECURSE BLUEPRINT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/*.blp)

# Compile Blueprint files to UI files if blueprint-compiler is available
set(SIGNER_COMPILED_UI_FILES)
if(BLUEPRINT_COMPILER AND BLUEPRINT_SOURCES)
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
    list(APPEND SIGNER_COMPILED_UI_FILES ${UI_FILE})
  endforeach()
  
  # Create a target for all Blueprint compilations
  add_custom_target(gnostr_signer_blueprints ALL DEPENDS ${SIGNER_COMPILED_UI_FILES})
endif()

# Main GTK4 application (UI shell)
file(GLOB APP_SRC CONFIGURE_DEPENDS src/*.c src/ui/*.c src/ui/sheets/*.c src/ui/widgets/*.c)
if (NOT APP_SRC)
  set(APP_SRC src/main_app.c)
endif()
set(APP_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml)
set(APP_GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
# Rebuild resources.c whenever any referenced UI changes
file(GLOB_RECURSE APP_UI_FILES ${CMAKE_CURRENT_SOURCE_DIR}/data/*.ui)
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
# GObject NIP-46 wrappers (GNostrNip46Client, GNostrNip46Bunker)
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nostr-gobject/include)
target_sources(${APP_SIGNER_BIN} PRIVATE
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_nip46_client.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_nip46_bunker.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr-enums.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr-error.c
)
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
# Ensure library directories are passed to linker (macOS fix)
target_link_directories(${APP_SIGNER_BIN} PRIVATE ${GLIB_LIBRARY_DIRS})
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
# NIP-44 for encrypted DMs (used by GNostrKeys wrapper)
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip44/include)
if (TARGET nostr_nip44_core)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE nostr_nip44_core)
endif()
# OpenSSL for RAND_bytes (used by GNostrKeys wrapper)
if (OPENSSL_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE OpenSSL::Crypto)
endif()
# Note: nostr core is already transitively linked via nostr_nip46_core
# Do NOT link nostr directly here - it causes ODR violation with nostr_json.so

# GObject wrapper headers (GNostrKeys, GNostrNip19, GNostrNip49, GNostrBip39)
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR}/nostr-gobject/include)
# Compile GObject wrapper sources into the signer (hq-7p5m6)
target_sources(${APP_SIGNER_BIN} PRIVATE
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_nip49.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_bip39.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_nip19.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_keys.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr_event.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr-error.c
  ${CMAKE_SOURCE_DIR}/nostr-gobject/src/nostr-enums.c)

# Include project root so we can include gnome/seahorse/secret_store.h from the app
target_include_directories(${APP_SIGNER_BIN} PRIVATE ${CMAKE_SOURCE_DIR})

# If Seahorse helper lib is built, link it to enable secret deletion helpers
if (TARGET gnostr-secret)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE gnostr-secret)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_LIBSECRET=1)
endif()

# Note: GNOSTR_HAVE_LIBSECRET is only defined when gnostr-secret target exists
# (above), because settings_page.c uses functions from that library.
# We still link libsecret directly for secret-storage.c basic operations.
if (LIBSECRET_FOUND AND NOT TARGET gnostr-secret)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::LIBSECRET)
  message(STATUS "gnostr-signer: libsecret linked (without gnostr-secret helpers)")
elseif(APPLE)
  # macOS: Link Security.framework for Keychain support
  find_library(SECURITY_FRAMEWORK Security REQUIRED)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
  message(STATUS "gnostr-signer: Security.framework linked for Keychain support")
elseif(WIN32)
  # Windows: Use DPAPI for secure credential storage
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_USE_WINDOWS_CREDENTIAL_STORE=1)
  message(STATUS "gnostr-signer: Windows Credential Manager (DPAPI) will be used")
endif()

# Link libsodium for secure memory and Ed25519 if available
if (SODIUM_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::SODIUM)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_SODIUM=1 HAVE_SODIUM=1)
endif()

# Link libsecp256k1 for direct Schnorr operations if available
if (SECP256K1_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::SECP256K1)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE HAVE_SECP256K1=1)
endif()

# Link p11-kit for PKCS#11 HSM support if available
if (GNOSTR_SIGNER_WITH_PKCS11 AND P11KIT_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::P11KIT)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_PKCS11=1)
  message(STATUS "gnostr-signer: PKCS#11 HSM support enabled")
endif()

# Link hidapi for hardware wallet support if available
if (GNOSTR_SIGNER_WITH_HW_WALLET AND HIDAPI_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::HIDAPI)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE GNOSTR_HAVE_HIDAPI=1)
  message(STATUS "gnostr-signer: Hardware wallet support enabled (Ledger/Trezor)")
endif()

# Link libqrencode for QR code generation if available
if (QRENCODE_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::QRENCODE)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE HAVE_QRENCODE=1)
  message(STATUS "gnostr-signer: QR code generation enabled")
endif()

# Link zbar for QR code scanning if available
if (ZBAR_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::ZBAR)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE HAVE_ZBAR=1)
  message(STATUS "gnostr-signer: QR code scanning enabled")
endif()

# Link GStreamer for camera-based QR scanning if available
if (GST_FOUND)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE PkgConfig::GST)
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE HAVE_GSTREAMER=1)
  message(STATUS "gnostr-signer: Camera QR scanning enabled")
endif()

# Link math library on Linux (for fmin, fmax, etc.)
if (UNIX AND NOT APPLE)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE m)
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

# Compile GSettings schema for development/testing
set(SIGNER_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(SIGNER_SCHEMA_FILE ${SIGNER_SCHEMA_DIR}/org.gnostr.Signer.gschema.xml)
set(SIGNER_SCHEMA_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gschemas.compiled)

find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS)
  add_custom_command(
    OUTPUT ${SIGNER_SCHEMA_OUTPUT}
    COMMAND ${GLIB_COMPILE_SCHEMAS}
            ${SIGNER_SCHEMA_DIR}
            --targetdir=${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${SIGNER_SCHEMA_FILE}
    COMMENT "Compiling GSettings schemas for gnostr-signer"
    VERBATIM
  )
  add_custom_target(gnostr-signer-schemas ALL DEPENDS ${SIGNER_SCHEMA_OUTPUT})
  add_dependencies(${APP_SIGNER_BIN} gnostr-signer-schemas)
else()
  message(WARNING "glib-compile-schemas not found - GSettings schemas will not be compiled")
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
# NIP-07 Native Messaging Host
# ==============================================================================
# Native messaging host for browser extension support (Chrome/Firefox)
option(BUILD_NATIVE_HOST "Build NIP-07 native messaging host" ON)
if (BUILD_NATIVE_HOST)
  add_subdirectory(native-host)
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
# Windows Installer Support
# ==============================================================================
if(WIN32)
  option(BUILD_WINDOWS_INSTALLER "Build Windows NSIS installer" OFF)

  # Windows-specific compile definitions
  target_compile_definitions(${APP_SIGNER_BIN} PRIVATE
    GNOSTR_PLATFORM_WINDOWS=1
    _WIN32_WINNT=0x0A00  # Windows 10+
  )
  target_compile_definitions(gnostr-signer-daemon PRIVATE
    GNOSTR_PLATFORM_WINDOWS=1
    _WIN32_WINNT=0x0A00
  )

  # Link Windows credential storage (DPAPI)
  target_link_libraries(${APP_SIGNER_BIN} PRIVATE crypt32 advapi32)
  message(STATUS "gnostr-signer: Windows DPAPI linked for credential storage")

  # Enable Named Pipes IPC on Windows (instead of D-Bus)
  target_compile_definitions(gnostr-signer-daemon PRIVATE GNOSTR_USE_NAMED_PIPES=1)

  if(BUILD_WINDOWS_INSTALLER)
    # Application version for installer
    set(GNOSTR_SIGNER_VERSION "1.0.0" CACHE STRING "Application version for Windows installer")

    # Path to packaging files
    set(WINDOWS_PACKAGING_DIR ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows)

    # Find NSIS
    find_program(MAKENSIS_EXECUTABLE makensis
      HINTS
        "C:/Program Files (x86)/NSIS"
        "C:/Program Files/NSIS"
        "$ENV{PROGRAMFILES}/NSIS"
        "$ENV{PROGRAMFILES\(x86\)}/NSIS"
    )

    if(MAKENSIS_EXECUTABLE)
      # Custom target to bundle dependencies
      add_custom_target(windows-bundle-deps
        COMMAND ${CMAKE_COMMAND} -E echo "Bundling Windows dependencies..."
        COMMAND powershell -ExecutionPolicy Bypass -File
          "${WINDOWS_PACKAGING_DIR}/bundle-deps.ps1"
          -BuildDir "${CMAKE_BINARY_DIR}"
          -OutputDir "${CMAKE_BINARY_DIR}/deps"
        DEPENDS ${APP_SIGNER_BIN} gnostr-signer-daemon
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Bundling GTK4 and dependencies for Windows installer"
        VERBATIM
      )

      # Custom target to build NSIS installer
      add_custom_target(windows-installer
        COMMAND ${CMAKE_COMMAND} -E echo "Building Windows NSIS installer..."
        COMMAND ${MAKENSIS_EXECUTABLE}
          /DVERSION=${GNOSTR_SIGNER_VERSION}
          /DARCH=x64
          /DBUILD_DIR=${CMAKE_BINARY_DIR}
          /DDEPS_DIR=${CMAKE_BINARY_DIR}/deps
          "${WINDOWS_PACKAGING_DIR}/gnostr-signer.nsi"
        DEPENDS windows-bundle-deps
        WORKING_DIRECTORY ${WINDOWS_PACKAGING_DIR}
        COMMENT "Creating Windows NSIS installer"
        VERBATIM
      )

      message(STATUS "Windows installer support enabled. Available targets:")
      message(STATUS "  - windows-bundle-deps: Bundle GTK4 runtime and dependencies")
      message(STATUS "  - windows-installer: Build NSIS installer package")
    else()
      message(WARNING "NSIS not found - Windows installer targets disabled")
      message(STATUS "Install NSIS from https://nsis.sourceforge.io/ to enable")
    endif()
  endif()
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
