# CMakeLists.txt for gnostr-signer-native (NIP-07 Native Messaging Host)

set(NATIVE_HOST_BIN "gnostr-signer-native")

# Source files
set(NATIVE_HOST_SRC
  main_native_host.c
  native_messaging.c
)

# Create executable
add_executable(${NATIVE_HOST_BIN} ${NATIVE_HOST_SRC})
target_compile_features(${NATIVE_HOST_BIN} PRIVATE c_std_11)

# Include directories
target_include_directories(${NATIVE_HOST_BIN} PRIVATE
  ${CMAKE_SOURCE_DIR}/nips/nip55l/include
  ${CMAKE_SOURCE_DIR}/nips/nip19/include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

# Link libraries
target_link_libraries(${NATIVE_HOST_BIN} PRIVATE
  PkgConfig::GLIB
  PkgConfig::JSON_GLIB
  nostr_nip55l_core
)

# Link NIP-19 for bech32 encoding
if(TARGET nip19)
  target_link_libraries(${NATIVE_HOST_BIN} PRIVATE nip19)
endif()

# Link nostr core for keys
if(TARGET nostr)
  target_link_libraries(${NATIVE_HOST_BIN} PRIVATE nostr)
endif()

# Link NIP-04 for encryption
if(TARGET nip04)
  target_link_libraries(${NATIVE_HOST_BIN} PRIVATE nip04)
endif()

# Link libsodium for secure memory if available
if(SODIUM_FOUND)
  target_link_libraries(${NATIVE_HOST_BIN} PRIVATE PkgConfig::SODIUM)
  target_compile_definitions(${NATIVE_HOST_BIN} PRIVATE GNOSTR_HAVE_SODIUM=1)
endif()

# Platform-specific settings
if(APPLE)
  find_library(SECURITY_FRAMEWORK Security)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(SECURITY_FRAMEWORK AND COREFOUNDATION_FRAMEWORK)
    target_link_libraries(${NATIVE_HOST_BIN} PRIVATE
      ${SECURITY_FRAMEWORK}
      ${COREFOUNDATION_FRAMEWORK}
    )
  endif()
endif()

if(UNIX AND NOT APPLE)
  # Link math library on Linux
  target_link_libraries(${NATIVE_HOST_BIN} PRIVATE m)
endif()

# Install binary
install(TARGETS ${NATIVE_HOST_BIN}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install manifest templates
install(FILES
  manifests/chrome/org.gnostr.signer.json
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/gnostr-signer/native-host/manifests/chrome
)

install(FILES
  manifests/firefox/org.gnostr.signer.json
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/gnostr-signer/native-host/manifests/firefox
)

# Install install script
install(PROGRAMS install.sh
  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/gnostr-signer/native-host
)

# Platform-specific manifest installation for development/local builds
if(APPLE)
  # macOS: User-level installation for development
  set(CHROME_MANIFEST_DIR "$ENV{HOME}/Library/Application Support/Google/Chrome/NativeMessagingHosts")
  set(CHROMIUM_MANIFEST_DIR "$ENV{HOME}/Library/Application Support/Chromium/NativeMessagingHosts")
  set(FIREFOX_MANIFEST_DIR "$ENV{HOME}/Library/Application Support/Mozilla/NativeMessagingHosts")
elseif(UNIX)
  # Linux: User-level installation for development
  set(CHROME_MANIFEST_DIR "$ENV{HOME}/.config/google-chrome/NativeMessagingHosts")
  set(CHROMIUM_MANIFEST_DIR "$ENV{HOME}/.config/chromium/NativeMessagingHosts")
  set(FIREFOX_MANIFEST_DIR "$ENV{HOME}/.mozilla/native-messaging-hosts")
endif()

# Custom target for local development installation
if(UNIX)
  add_custom_target(install-native-host-dev
    COMMAND ${CMAKE_COMMAND} -E echo "Installing native messaging host for development..."
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CHROME_MANIFEST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CHROMIUM_MANIFEST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FIREFOX_MANIFEST_DIR}"
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/install.sh --user --firefox-only
    DEPENDS ${NATIVE_HOST_BIN}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Installing native messaging host manifests for development"
    VERBATIM
  )
endif()
