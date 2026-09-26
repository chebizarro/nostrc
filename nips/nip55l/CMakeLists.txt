cmake_minimum_required(VERSION 3.22)

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

# Optionally build GLib DBus service if GIO is available and NIP55L is enabled
# (the GLib/DBus layer requires Unix D-Bus which isn't available on Windows)
include(CheckIncludeFile)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND AND ENABLE_NIP55L)
  pkg_check_modules(GIO QUIET IMPORTED_TARGET gio-2.0>=2.50 gobject-2.0>=2.50)
endif()

if (GIO_FOUND AND ENABLE_NIP55L)
  # Generate DBus glue from XML using gdbus-codegen
  find_program(GDBUS_CODEGEN gdbus-codegen)
  if (NOT GDBUS_CODEGEN)
    message(FATAL_ERROR "gdbus-codegen not found; required to build NIP-55L GLib service/client")
  endif()

  set(DBUS_XML ${CMAKE_CURRENT_SOURCE_DIR}/dbus/org.nostr.Signer.xml)
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
  # Generated gdbus code may trip -Wpedantic in some GCC versions; don't fail build on it
  if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${GENERATED_DBUS_C} PROPERTIES COMPILE_OPTIONS "-Wno-error=pedantic")
  endif()
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

  # D-Bus activation for org.nostr.Signer is owned by apps/gnostr-signer
  # (its daemon exports this same GLib service). Installing a second
  # org.nostr.Signer.service here would collide on the same path, so the
  # standalone daemon's activation file is opt-in for headless installs that
  # ship no gnostr-signer. The template is configured, never copied: an
  # installed file must carry an absolute Exec= path, not a CMake literal.
  if (ENABLE_NIP55L_STANDALONE_ACTIVATION)
    include(GNUInstallDirs)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/dbus/org.nostr.Signer.service.in
                   ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Signer.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Signer.service
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/services)
    if (BUILD_TESTING)
      add_test(NAME nip55l_dbus_service_configured
               COMMAND ${CMAKE_COMMAND}
                       -DSERVICE_FILE=${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Signer.service
                       -P ${CMAKE_SOURCE_DIR}/cmake/CheckDbusServiceFile.cmake)
    endif()
  endif()
endif()

# The binary NIP-44 lane needs no D-Bus to be exercised: the service handlers
# are pass-throughs over these ops, so the ops are where the guarantee lives.
if (BUILD_TESTING)
  add_executable(test_nip55l_nip44_b64 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip55l_nip44_b64.c)
  target_link_libraries(test_nip55l_nip44_b64 PRIVATE nostr_nip55l_core nostr_nip44_core nostr)
  add_test(NAME nip55l_nip44_b64 COMMAND test_nip55l_nip44_b64)
  set_tests_properties(nip55l_nip44_b64 PROPERTIES LABELS "nip55l;signer;unit;nip44-binary" TIMEOUT 60)

  # Core contract tests for the D1.a / D2 landings: SignEvent returns the
  # complete signed event JSON, GetRelays reads explicit per-user config only.
  # Keys are passed as explicit hex/nsec selectors so no test reaches
  # libsecret, the Keychain, or a session bus. Fuzz harness and a real-service
  # D-Bus contract test are follow-ups (see the nip55l/README roadmap).
  foreach(t sign_event_json getrelays)
    add_executable(test_nip55l_${t} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${t}.c)
    target_link_libraries(test_nip55l_${t} PRIVATE nostr_nip55l_core nostr_nip44_core nostr)
    target_compile_options(test_nip55l_${t} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME nip55l_${t} COMMAND test_nip55l_${t})
    set_tests_properties(nip55l_${t} PROPERTIES LABELS "nip55l;signer;unit" TIMEOUT 60)
  endforeach()
endif()

install(TARGETS nostr_nip55l_core)
install(DIRECTORY ${NIP55L_INC}/ DESTINATION include/nostr/nip55l)
