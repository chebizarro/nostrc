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
  # libsecret, the Keychain, or a session bus. Fuzz harnesses and the
  # real-service D-Bus contract test wire below (nostrc-tf3b).
  foreach(t sign_event_json getrelays)
    add_executable(test_nip55l_${t} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${t}.c)
    target_link_libraries(test_nip55l_${t} PRIVATE nostr_nip55l_core nostr_nip44_core nostr)
    target_compile_options(test_nip55l_${t} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME nip55l_${t} COMMAND test_nip55l_${t})
    set_tests_properties(nip55l_${t} PROPERTIES LABELS "nip55l;signer;unit" TIMEOUT 60)
  endforeach()

  # --- Real-service D-Bus contract test (nostrc-tf3b) -----------------------
  # Spins up a private session bus with GTestDBus, spawns the actual
  # nostr-signer-daemon on it, and drives StoreKey / GetPublicKey / SignEvent
  # / NIP44EncryptB64 / NIP44DecryptB64 / GetRelays / ClearKey through the
  # generated gdbus glue. Requires the GLib service + the daemon binary
  # (so gated on GIO_FOUND + ENABLE_NIP55L, matching the daemon target).
  if (GIO_FOUND AND ENABLE_NIP55L)
    add_executable(test_nip55l_dbus_contract
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_signer_dbus_contract.c)
    target_link_libraries(test_nip55l_dbus_contract PRIVATE
      nostr_nip55l_core nostr nostr_nip44_core PkgConfig::GIO)
    target_compile_options(test_nip55l_dbus_contract PRIVATE
      -Wall -Wextra -Werror)
    # The test locates the daemon by absolute build path.
    target_compile_definitions(test_nip55l_dbus_contract PRIVATE
      "NIP55L_DAEMON_PATH=\"$<TARGET_FILE:nostr-signer-daemon>\"")
    add_dependencies(test_nip55l_dbus_contract nostr-signer-daemon)
    add_test(NAME nip55l_dbus_contract COMMAND test_nip55l_dbus_contract)
    set_tests_properties(nip55l_dbus_contract PROPERTIES
      LABELS "nip55l;signer;integration;dbus"
      TIMEOUT 120)
  endif()

  # --- libFuzzer harnesses + corpus replay (nostrc-tf3b) --------------------
  # Shape matches tests/CMakeLists.txt: with ENABLE_FUZZING_RUNTIME=ON the
  # target is a libFuzzer binary (Clang only); otherwise it is a deterministic
  # corpus-replay binary that links tests/fuzz_driver_main.c and can run
  # under ctest with either GCC or Clang.
  if (ENABLE_FUZZING)
    set(NIP55L_FUZZ_TARGETS
      relays             fuzz_relays.c              relays
      sign_event_json    fuzz_sign_event_json.c     sign_event
      nip44_b64          fuzz_nip44_b64.c           nip44_b64
      decrypt_zap_event  fuzz_decrypt_zap_event.c   zap_event
    )
    list(LENGTH NIP55L_FUZZ_TARGETS _nip55l_fuzz_len)
    math(EXPR _nip55l_fuzz_last "${_nip55l_fuzz_len} - 1")
    foreach(_i RANGE 0 ${_nip55l_fuzz_last} 3)
      list(GET NIP55L_FUZZ_TARGETS ${_i} _name)
      math(EXPR _j "${_i} + 1")
      math(EXPR _k "${_i} + 2")
      list(GET NIP55L_FUZZ_TARGETS ${_j} _src)
      list(GET NIP55L_FUZZ_TARGETS ${_k} _corpus)
      set(_tgt fuzz_nip55l_${_name})
      set(_src_path ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/${_src})
      set(_corpus_path ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${_corpus})
      if (ENABLE_FUZZING_RUNTIME)
        # Live libFuzzer binary: Clang + libclang_rt.fuzzer required.
        if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
          message(FATAL_ERROR "ENABLE_FUZZING_RUNTIME requires Clang for nip55l fuzz targets")
        endif()
        add_executable(${_tgt} ${_src_path})
        target_link_libraries(${_tgt} PRIVATE nostr_nip55l_core nostr_nip44_core nostr)
        target_compile_options(${_tgt} PRIVATE
          -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${_tgt} PRIVATE
          -fsanitize=fuzzer,address,undefined)
      else()
        # Corpus-replay binary: same fuzz object linked with the shared
        # deterministic driver from tests/fuzz_driver_main.c. Runs under
        # ctest and stays compatible with GCC.
        add_executable(${_tgt}_replay ${_src_path}
                       ${CMAKE_SOURCE_DIR}/tests/fuzz_driver_main.c)
        target_link_libraries(${_tgt}_replay PRIVATE
          nostr_nip55l_core nostr_nip44_core nostr)
        if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
          target_compile_options(${_tgt}_replay PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
          target_link_options(${_tgt}_replay PRIVATE
            -fsanitize=address,undefined)
        endif()
        add_test(NAME nip55l_fuzz_${_name}_replay
                 COMMAND ${_tgt}_replay ${_corpus_path})
        set_tests_properties(nip55l_fuzz_${_name}_replay PROPERTIES
          LABELS "nip55l;signer;fuzz;replay"
          TIMEOUT 120)
      endif()
    endforeach()
  endif()
endif()

install(TARGETS nostr_nip55l_core)
install(DIRECTORY ${NIP55L_INC}/ DESTINATION include/nostr/nip55l)
