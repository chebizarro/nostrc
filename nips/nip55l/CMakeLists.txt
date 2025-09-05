cmake_minimum_required(VERSION 3.30)

set(NIP55L_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP55L_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/signer_ops.c
)

add_library(nostr_nip55l_core ${NIP55L_CORE_SRC})
target_include_directories(nostr_nip55l_core PUBLIC
  ${NIP55L_INC}
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_BINARY_DIR}/libnostr/generated
  ${CMAKE_SOURCE_DIR}/nips/nip04/include
  ${CMAKE_SOURCE_DIR}/nips/nip44/include
  ${CMAKE_SOURCE_DIR}/nips/nip19/include
)
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
target_link_libraries(nostr_nip55l_core PUBLIC nostr nip04 nip19 nostr_nip44_core OpenSSL::Crypto ${SECP256K1_LIB})

# Optional libsecret for key storage
if (PkgConfig_FOUND)
  # On macOS prefer Keychain; skip libsecret even if present
  if (NOT APPLE)
    pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
    if (LIBSECRET_FOUND)
      target_link_libraries(nostr_nip55l_core PUBLIC PkgConfig::LIBSECRET)
      target_compile_definitions(nostr_nip55l_core PRIVATE -DNIP55L_HAVE_LIBSECRET)
    endif()
  endif()
endif()

# macOS Keychain fallback when libsecret is not present
if (APPLE)
  find_library(SECURITY_FRAMEWORK Security)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
  if (SECURITY_FRAMEWORK AND COREFOUNDATION_FRAMEWORK)
    target_link_libraries(nostr_nip55l_core PUBLIC ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    target_compile_definitions(nostr_nip55l_core PRIVATE -DNIP55L_HAVE_KEYCHAIN)
    message(STATUS "NIP-55L: Using macOS Keychain for secure storage")
  endif()
endif()

# Optionally build GLib DBus service if GIO is available
include(CheckIncludeFile)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(GIO QUIET IMPORTED_TARGET gio-2.0>=2.50 gobject-2.0>=2.50)
endif()

if (GIO_FOUND)
  # Generate DBus glue from XML using gdbus-codegen
  find_program(GDBUS_CODEGEN gdbus-codegen)
  if (NOT GDBUS_CODEGEN)
    message(FATAL_ERROR "gdbus-codegen not found; required to build NIP-55L GLib service/client")
  endif()

  set(DBUS_XML ${CMAKE_CURRENT_SOURCE_DIR}/dbus/com.nostr.Signer.xml)
  set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
  file(MAKE_DIRECTORY ${GENERATED_DIR})

  set(GENERATED_DBUS_C ${GENERATED_DIR}/signer_dbus.c)
  set(GENERATED_DBUS_H ${GENERATED_DIR}/signer_dbus.h)

  add_custom_command(
    OUTPUT ${GENERATED_DBUS_C} ${GENERATED_DBUS_H}
    COMMAND ${GDBUS_CODEGEN}
            --interface-prefix org.nostr.
            --c-namespace Nostr
            --generate-c-code ${GENERATED_DIR}/signer_dbus
            ${DBUS_XML}
    DEPENDS ${DBUS_XML}
    VERBATIM)

  add_library(nostr_nip55l_glib
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/signer_service_g.c
    ${GENERATED_DBUS_C}
  )
  target_include_directories(nostr_nip55l_glib
    PUBLIC
      ${NIP55L_INC}
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${GENERATED_DIR}
  )
  # Fail-fast hygiene for GLib signer target
  target_compile_options(nostr_nip55l_glib PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(nostr_nip55l_glib PUBLIC ${NIP55L_INC})
  target_compile_definitions(nostr_nip55l_glib PRIVATE -DNIP55L_HAVE_GIO)
  target_link_libraries(nostr_nip55l_glib PUBLIC nostr_nip55l_core PkgConfig::GIO)
  # Ensure NIP-04 secure functions are available to executables linking the GLib target
  target_link_libraries(nostr_nip55l_glib PUBLIC nip04)

  add_executable(nostr-signer-daemon ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/daemon_main.c)
  target_link_libraries(nostr-signer-daemon PRIVATE nostr_nip55l_glib PkgConfig::GIO)

  # CLI tool
  add_executable(nostr-signer-cli ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/signer_cli.c)
  target_link_libraries(nostr-signer-cli PRIVATE PkgConfig::GIO)

  install(TARGETS nostr_nip55l_glib nostr-signer-daemon nostr-signer-cli)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/dbus/org.nostr.Signer.service.in
          DESTINATION share/dbus-1/services RENAME org.nostr.Signer.service)
endif()

install(TARGETS nostr_nip55l_core)
install(DIRECTORY ${NIP55L_INC}/ DESTINATION include/nostr/nip55l)
