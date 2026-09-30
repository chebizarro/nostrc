cmake_minimum_required(VERSION 3.22)

set(NIP55L_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP55L_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/signer_ops.c
)

add_library(nostr_nip55l_core ${NIP55L_CORE_SRC})

# nostrc-o8mx: real SONAMEs for the shared build (libnostr_nip55l_core.so.0
# -> .so.0.4.0, same for _glib), like libnip19/libnip34 (nostrc-4c0o). The
# version comes from NOSTR_NIP55L_VERSION_* in signer_ops.h, the component's
# single source of truth. Pre-1.0: SOVERSION changes on any ABI break.
file(STRINGS ${NIP55L_INC}/nostr/nip55l/signer_ops.h _nip55l_ver
     REGEX "^#define NOSTR_NIP55L_VERSION_(MAJOR|MINOR|PATCH) [0-9]+")
foreach(_part MAJOR MINOR PATCH)
  string(REGEX REPLACE ".*NOSTR_NIP55L_VERSION_${_part} ([0-9]+).*" "\\1"
         NIP55L_VERSION_${_part} "${_nip55l_ver}")
endforeach()
set(NIP55L_VERSION "${NIP55L_VERSION_MAJOR}.${NIP55L_VERSION_MINOR}.${NIP55L_VERSION_PATCH}")
if (NOT NIP55L_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "nip55l: cannot parse NOSTR_NIP55L_VERSION_* from signer_ops.h")
endif()
set(NIP55L_SOVERSION 0)
set_target_properties(nostr_nip55l_core PROPERTIES
  VERSION ${NIP55L_VERSION} SOVERSION ${NIP55L_SOVERSION})
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
      # nostrc-bml6: the key schema is owned by gnome/seahorse (gnostr-secret).
      # PRIVATE so its usage requirements do not leak into consumers.
      if (NOT TARGET gnostr-secret)
        add_subdirectory(${CMAKE_SOURCE_DIR}/gnome/seahorse
                         ${CMAKE_BINARY_DIR}/gnome/seahorse)
      endif()
      target_link_libraries(nostr_nip55l_core PRIVATE gnostr-secret)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/signer_caller.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/signer_nip5f.c
    ${GENERATED_DBUS_C}
  )
  # The NIP-5F socket, gated like the D-Bus methods (nostrc-q23h).
  if (TARGET nostr_nip5f_core)
    target_link_libraries(nostr_nip55l_glib PRIVATE nostr_nip5f_core)
    target_compile_definitions(nostr_nip55l_glib PRIVATE NIP55L_HAVE_NIP5F)
  endif()
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
  set_target_properties(nostr_nip55l_glib PROPERTIES
    VERSION ${NIP55L_VERSION} SOVERSION ${NIP55L_SOVERSION})
  # Fail-fast hygiene for GLib signer target
  target_compile_options(nostr_nip55l_glib PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_include_directories(nostr_nip55l_glib PUBLIC ${NIP55L_INC})
  target_compile_definitions(nostr_nip55l_glib PRIVATE -DNIP55L_HAVE_GIO)
  target_link_libraries(nostr_nip55l_glib PUBLIC nostr_nip55l_core PkgConfig::GIO)

  # --- Caller identity and trust (nip55l 0.4.0: nostrc-phk4, nostrc-1e31) ---
  # ProcessFD from GetConnectionCredentials (pidfd) needs GUnixFDList.
  pkg_check_modules(NIP55L_GIOUNIX QUIET IMPORTED_TARGET gio-unix-2.0)
  if (NIP55L_GIOUNIX_FOUND)
    target_link_libraries(nostr_nip55l_glib PRIVATE PkgConfig::NIP55L_GIOUNIX)
    target_compile_definitions(nostr_nip55l_glib PRIVATE NIP55L_HAVE_GIO_UNIX)
  endif()
  include(GNUInstallDirs)
  # Executables that may name the web origin they act for (the browser
  # bridge) and executables that may answer ApproveRequest (the approval UI).
  # Colon-separated absolute paths; matched by path AND device/inode.
  set(NIP55L_ORIGIN_BRIDGE_PATHS "${CMAKE_INSTALL_FULL_LIBEXECDIR}/nostr-signer-webext-host"
      CACHE STRING "Executables allowed to assert a web origin as app_id (colon-separated)")
  set(NIP55L_APPROVER_PATHS "${CMAKE_INSTALL_FULL_BINDIR}/gnostr-signer"
      CACHE STRING "Executables allowed to call ApproveRequest (colon-separated)")
  set(NIP55L_APPROVER_FLATPAK_ID "org.gnostr.Signer"
      CACHE STRING "Flatpak app id allowed to call ApproveRequest (empty = none)")
  # Bus name the approval UI owns; without an owner, calls that need a prompt
  # fail at once instead of waiting for an answer that cannot come.
  set(NIP55L_APPROVER_BUS_NAME "org.gnostr.Signer"
      CACHE STRING "Well-known name of the approval UI (empty = do not check)")
  # First-party services that run without a UI and read the npub / relays
  # (nostr-homed session helpers) or open the secrets envelope at login
  # (nostr-homectl). "<kind>[,<kind>]@<exe>[:<exe>]" groups, "+"-separated.
  set(_nip55l_bin "${CMAKE_INSTALL_FULL_BINDIR}")
  set(_nip55l_readers "${_nip55l_bin}/nostr-homectl:${_nip55l_bin}/nostrfs:${_nip55l_bin}/nostr-home-fuse:${_nip55l_bin}/nostr-home-syncd:${_nip55l_bin}/nostr-home-fetch:${_nip55l_bin}/nostr-homed-provision:${_nip55l_bin}/nostr-home-status:${CMAKE_INSTALL_FULL_LIBEXECDIR}/nostr-notify-daemon")
  set(NIP55L_DEFAULT_GRANTS
      "get_public_key,get_relays@${_nip55l_readers}+nip44_decrypt@${_nip55l_bin}/nostr-homectl"
      CACHE STRING "Built-in grants for first-party headless services")
  target_compile_definitions(nostr_nip55l_glib PRIVATE
    "NIP55L_ORIGIN_BRIDGE_PATHS=\"${NIP55L_ORIGIN_BRIDGE_PATHS}\""
    "NIP55L_APPROVER_PATHS=\"${NIP55L_APPROVER_PATHS}\""
    "NIP55L_APPROVER_FLATPAK_ID=\"${NIP55L_APPROVER_FLATPAK_ID}\""
    "NIP55L_APPROVER_BUS_NAME=\"${NIP55L_APPROVER_BUS_NAME}\""
    "NIP55L_DEFAULT_GRANTS=\"${NIP55L_DEFAULT_GRANTS}\"")
  # Test builds only: NOSTR_SIGNER_TEST_ORIGIN_BRIDGES / NOSTR_SIGNER_TEST_APPROVERS
  # replace the two executable lists so CTest can drive the real daemon from
  # build-tree binaries. Distro packages configure BUILD_TESTING=OFF.
  if (NOT DEFINED NIP55L_TEST_TRUST_ENV)
    set(NIP55L_TEST_TRUST_ENV ${BUILD_TESTING})
  endif()
  option(NIP55L_TEST_TRUST_ENV "Honour NOSTR_SIGNER_TEST_* trust overrides (test builds)" ${NIP55L_TEST_TRUST_ENV})
  if (NIP55L_TEST_TRUST_ENV)
    target_compile_definitions(nostr_nip55l_glib PRIVATE NIP55L_TEST_TRUST_ENV=1)
  endif()
  # Ensure NIP-04 secure functions are available to executables linking the GLib target
  target_link_libraries(nostr_nip55l_glib PUBLIC nip04)

  # The interface definition, for clients' gdbus-codegen / introspection.
  # This file is the only copy in the tree (nostrc-56id).
  install(FILES ${DBUS_XML} DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)

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

  # macOS Keychain import of pre-e5nz GNostr client keys (nostrc-de9h). The
  # migration is compiled into the test with NIP55L_KEYCHAIN_TEST_HOOKS so it
  # runs against a throwaway keychain file, never the login keychain.
  if (APPLE AND SECURITY_FRAMEWORK AND COREFOUNDATION_FRAMEWORK)
    add_executable(test_nip55l_keychain_migration
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keychain_migration.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core/signer_ops.c)
    target_include_directories(test_nip55l_keychain_migration PRIVATE
      $<TARGET_PROPERTY:nostr_nip55l_core,INCLUDE_DIRECTORIES>)
    target_compile_definitions(test_nip55l_keychain_migration PRIVATE
      NIP55L_HAVE_KEYCHAIN NIP55L_KEYCHAIN_TEST_HOOKS)
    target_link_libraries(test_nip55l_keychain_migration PRIVATE
      nostr nip04 nip19 nostr_nip44_core OpenSSL::Crypto ${SECP256K1_LIB}
      ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    target_compile_options(test_nip55l_keychain_migration PRIVATE -Wall -Wextra -Werror)
    add_test(NAME nip55l_keychain_migration COMMAND test_nip55l_keychain_migration)
    set_tests_properties(nip55l_keychain_migration PROPERTIES
      LABELS "nip55l;signer;unit;keyring" TIMEOUT 60)
  endif()

  # Unified libsecret key schema (nostrc-bml6): schema shape, identity and
  # legacy attribute mapping, Seahorse label. Pure functions, no keyring.
  if (TARGET gnostr-secret)
    add_executable(test_nip55l_keyring_schema
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keyring_schema.c)
    target_link_libraries(test_nip55l_keyring_schema PRIVATE gnostr-secret)
    target_compile_options(test_nip55l_keyring_schema PRIVATE -Wall -Wextra -Werror)
    add_test(NAME nip55l_keyring_schema COMMAND test_nip55l_keyring_schema)
    set_tests_properties(nip55l_keyring_schema PROPERTIES
      LABELS "nip55l;signer;unit;keyring" TIMEOUT 60)
  endif()

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
      nostr_nip55l_core nostr nostr_nip44_core PkgConfig::GIO nostr_nip55l_glib)
    # signer_caller.h: the test derives its own principal the daemon's way.
    target_include_directories(test_nip55l_dbus_contract PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/glib)
    target_compile_options(test_nip55l_dbus_contract PRIVATE
      -Wall -Wextra -Werror)
    # The test locates the daemon by absolute build path.
    target_compile_definitions(test_nip55l_dbus_contract PRIVATE
      "NIP55L_DAEMON_PATH=\"$<TARGET_FILE:nostr-signer-daemon>\"")
    add_dependencies(test_nip55l_dbus_contract nostr-signer-daemon)
    # Phase 3 (nostrc-bml6): real gnome-keyring on the private bus, when
    # the signer was built against libsecret.
    if (TARGET gnostr-secret)
      target_link_libraries(test_nip55l_dbus_contract PRIVATE gnostr-secret)
      target_compile_definitions(test_nip55l_dbus_contract PRIVATE NIP55L_TEST_HAVE_LIBSECRET)
    endif()
    add_test(NAME nip55l_dbus_contract COMMAND test_nip55l_dbus_contract)
    set_tests_properties(nip55l_dbus_contract PROPERTIES
      LABELS "nip55l;signer;integration;dbus"
      TIMEOUT 120)
    # nostrc-oauv: a check that fails with the daemon and private bus up ends
    # the test at once. Without the test's exit teardown the orphaned
    # dbus-daemon kept ctest's output open until the timeout, and on macOS
    # GTestDBus's watcher crashed in strlen(NULL) instead of killing it (on
    # Linux the watcher kills it and says "cleaning up pid").
    add_test(NAME nip55l_dbus_contract_fail_teardown COMMAND test_nip55l_dbus_contract)
    set_tests_properties(nip55l_dbus_contract_fail_teardown PROPERTIES
      ENVIRONMENT "NIP55L_TEST_FAIL_WITH_FIXTURE_UP=1"
      PASS_REGULAR_EXPRESSION "injected failure with the fixture up"
      FAIL_REGULAR_EXPRESSION "AddressSanitizer|cleaning up pid"
      LABELS "nip55l;signer;integration;dbus"
      TIMEOUT 30)
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
