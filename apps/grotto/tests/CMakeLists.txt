# grotto unit tests
# Uses GLib testing framework for consistency with GTK application

cmake_minimum_required(VERSION 3.22)
project(grotto-tests C)

enable_testing()

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

find_package(OpenSSL REQUIRED)

# Find nsync
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Common include directories
set(SIGNER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/nips/nip55l/include
    ${CMAKE_SOURCE_DIR}/nips/nip49/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
    ${CMAKE_SOURCE_DIR}/nips/nip44/include
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/nips/nip06/include
    ${CMAKE_SOURCE_DIR}/nips/nip04/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/nostr-gobject/include
    ${CMAKE_SOURCE_DIR}/libgo/include
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
)

# Common link libraries
set(SIGNER_TEST_LIBS
    PkgConfig::GLIB
    PkgConfig::JSON_GLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
    ${NSYNC_LIB}
    libnostr
    nostr_json
)

# Common link directories for pkg-config libraries
set(SIGNER_TEST_LINK_DIRS
    ${GLIB_LIBRARY_DIRS}
    ${JSON_GLIB_LIBRARY_DIRS}
    ${SECP256K1_LIBRARY_DIRS}
)

# Add NIP libraries if available
if(TARGET nip19)
    list(APPEND SIGNER_TEST_LIBS nip19)
endif()
if(TARGET nip06)
    list(APPEND SIGNER_TEST_LIBS nip06)
endif()
if(TARGET nostr_nip49_core)
    list(APPEND SIGNER_TEST_LIBS nostr_nip49_core)
endif()
if(TARGET nostr_nip49_glib)
    list(APPEND SIGNER_TEST_LIBS nostr_nip49_glib)
endif()
if(TARGET nip04)
    list(APPEND SIGNER_TEST_LIBS nip04)
endif()
if(TARGET nip44)
    list(APPEND SIGNER_TEST_LIBS nip44)
endif()
if(TARGET nostr_nip55l_core)
    list(APPEND SIGNER_TEST_LIBS nostr_nip55l_core)
endif()

# Sanitizer helper
function(apply_test_sanitizers target)
    if(GO_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    elseif(GO_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
    if(GO_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

# ==============================================================================
# Test: Crypto operations (key generation, signing, verification, NIP-49)
# ==============================================================================
add_executable(test-crypto test-crypto.c)
target_include_directories(test-crypto PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-crypto PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-crypto PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-crypto PRIVATE c_std_11)
apply_test_sanitizers(test-crypto)

add_test(NAME signer/crypto COMMAND test-crypto)
set_tests_properties(signer/crypto PROPERTIES
    TIMEOUT 60
    LABELS "signer;crypto;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Identity/Account store operations
# ==============================================================================
add_executable(test-identity test-identity.c)
target_include_directories(test-identity PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-identity PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-identity PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-identity PRIVATE c_std_11)
apply_test_sanitizers(test-identity)

add_test(NAME signer/identity COMMAND test-identity)
set_tests_properties(signer/identity PROPERTIES
    TIMEOUT 30
    LABELS "signer;identity;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Session/Settings manager
# ==============================================================================
add_executable(test-session test-session.c)
target_include_directories(test-session PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-session PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-session PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-session PRIVATE c_std_11)
apply_test_sanitizers(test-session)

add_test(NAME signer/session COMMAND test-session)
set_tests_properties(signer/session PROPERTIES
    TIMEOUT 30
    LABELS "signer;session;unit"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# signer/bunker's duplicate MockBunkerService was not production coverage.
# G05 (nostrc-u6v55.6) will register the broad real service suite after its rewrite.

# ==============================================================================
# Test: D-Bus interface integration tests
# Uses tests/common/nostrc-test-bus for isolated session bus testing
# Issue: nostrc-991
# ==============================================================================
add_executable(test-dbus test-dbus.c)
# nostrc-gcu4: a private bus bound to the test's lifetime, never GTestDBus.
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/NostrcTestBus.cmake)
target_link_libraries(test-dbus PRIVATE nostrc-test-bus)
target_include_directories(test-dbus PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-dbus PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-dbus PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-dbus PRIVATE c_std_11)
apply_test_sanitizers(test-dbus)

add_test(NAME signer/dbus COMMAND test-dbus)
set_tests_properties(signer/dbus PROPERTIES
    TIMEOUT 120
    LABELS "signer;dbus;integration"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;G_DEBUG=fatal-warnings;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Rate limiter (brute force attack prevention)
# Issue: nostrc-1g1
# ==============================================================================
# W31 R2: one-time move of the pre-rename per-user directories.
add_executable(test-legacy-dirs
    test-legacy-dirs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/legacy_dirs.c
)
target_include_directories(test-legacy-dirs PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-legacy-dirs PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-legacy-dirs PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-legacy-dirs PRIVATE c_std_11)
apply_test_sanitizers(test-legacy-dirs)
add_test(NAME signer/legacy-dirs COMMAND test-legacy-dirs)

add_executable(test-rate-limiter
    test-rate-limiter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rate-limiter.c
)
target_include_directories(test-rate-limiter PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-rate-limiter PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-rate-limiter PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-rate-limiter PRIVATE c_std_11)
apply_test_sanitizers(test-rate-limiter)

add_test(NAME signer/rate-limiter COMMAND test-rate-limiter)
set_tests_properties(signer/rate-limiter PROPERTIES
    TIMEOUT 30
    LABELS "signer;rate-limiter;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: UI components (window, dialogs, navigation, form validation)
# Uses GTK test utilities with mock widgets to avoid D-Bus/network dependencies
# ==============================================================================
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

add_executable(test-ui test-ui.c)
target_include_directories(test-ui PRIVATE ${SIGNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/tests/common)
target_link_libraries(test-ui PRIVATE
    ${SIGNER_TEST_LIBS}
    PkgConfig::GTK4
    PkgConfig::ADWAITA
)
target_link_directories(test-ui PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-ui PRIVATE c_std_11)
apply_test_sanitizers(test-ui)

add_test(NAME signer/ui COMMAND test-ui)
# On macOS there is no x11 backend; use the native backend instead.
if(APPLE)
    # Keep the test enabled on Apple, but force its built-in headless skip path in
    # environments where the Quartz backend cannot safely create/present windows.
    set(_UI_TEST_ENV "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto;GTK_A11Y=none;GNOSTR_SKIP_UI_TESTS=1")
else()
    set(_UI_TEST_ENV "GDK_BACKEND=x11;GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto;GTK_A11Y=none")
endif()
set_tests_properties(signer/ui PROPERTIES
    TIMEOUT 180
    LABELS "signer;ui;unit"
    ENVIRONMENT "${_UI_TEST_ENV}"
)

# ==============================================================================
# Test: approval dialog (nostrc-n98j) - the real approval_dialog.c and its
# template against stubbed store/session/keyboard-nav: the decision callback
# fires exactly once, and accounts_store_list() entries are freed only by the
# array. Same display requirements and skip policy as signer/ui.
# ==============================================================================
set(_APPROVAL_GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/approval-dialog.gresource.xml)
set(_APPROVAL_GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/approval-dialog-resources.c)
add_custom_command(
    OUTPUT ${_APPROVAL_GRESOURCE_C}
    COMMAND glib-compile-resources --target=${_APPROVAL_GRESOURCE_C} --generate-source
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/../data ${_APPROVAL_GRESOURCE_XML}
    DEPENDS ${_APPROVAL_GRESOURCE_XML} ${CMAKE_CURRENT_SOURCE_DIR}/../data/ui/approval-dialog.ui
    VERBATIM)

add_executable(test-approval-dialog
    test-approval-dialog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/approval_dialog.c
    ${_APPROVAL_GRESOURCE_C}
)
target_include_directories(test-approval-dialog PRIVATE ${SIGNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/tests/common)
target_link_libraries(test-approval-dialog PRIVATE PkgConfig::GTK4 PkgConfig::ADWAITA PkgConfig::GLIB)
target_compile_features(test-approval-dialog PRIVATE c_std_11)
apply_test_sanitizers(test-approval-dialog)

add_test(NAME signer/approval-dialog COMMAND test-approval-dialog)
set_tests_properties(signer/approval-dialog PROPERTIES
    TIMEOUT 60
    LABELS "signer;ui;unit"
    ENVIRONMENT "${_UI_TEST_ENV}"
)

# ==============================================================================
# Test: HSM provider (Hardware Security Module abstraction)
# Issue: nostrc-n30
# ==============================================================================
add_executable(test-hsm-provider
    test_hsm_provider.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/hsm_provider.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/hsm_provider_mock.c
)
target_include_directories(test-hsm-provider PRIVATE
    ${SIGNER_INCLUDE_DIRS}
)
target_link_libraries(test-hsm-provider PRIVATE
    ${SIGNER_TEST_LIBS}
    nostr_gobject
)
# Link nip19 for npub encoding
if(TARGET nip19)
    target_link_libraries(test-hsm-provider PRIVATE nip19)
endif()
target_link_directories(test-hsm-provider PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-hsm-provider PRIVATE c_std_11)
apply_test_sanitizers(test-hsm-provider)

add_test(NAME signer/hsm-provider COMMAND test-hsm-provider)
set_tests_properties(signer/hsm-provider PROPERTIES
    TIMEOUT 30
    LABELS "signer;hsm;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Core signer modules (secret_store, backup-recovery, session, relay, delegation)
# Uses mock implementations to test core functionality without external dependencies
# Issue: nostrc-ddh
# ==============================================================================
add_executable(test-core test-core.c)
target_include_directories(test-core PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-core PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-core PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-core PRIVATE c_std_11)
apply_test_sanitizers(test-core)

add_test(NAME signer/core COMMAND test-core)
set_tests_properties(signer/core PROPERTIES
    TIMEOUT 60
    LABELS "signer;core;unit;secret_store;backup;session;relay;delegation"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Secure memory management
# Issue: nostrc-ycd
# ==============================================================================
add_executable(test-secure-mem
    test-secure-mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/secure-mem.c
)
target_include_directories(test-secure-mem PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-secure-mem PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-secure-mem PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-secure-mem PRIVATE c_std_11)
apply_test_sanitizers(test-secure-mem)

add_test(NAME signer/secure-mem COMMAND test-secure-mem)
set_tests_properties(signer/secure-mem PROPERTIES
    TIMEOUT 30
    LABELS "signer;secure-mem;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Social Recovery (Shamir's Secret Sharing)
# Issue: nostrc-e9a
# ==============================================================================
add_executable(test-social-recovery
    test-social-recovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/social-recovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/secure-mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/backup-recovery.c
)
target_include_directories(test-social-recovery PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-social-recovery PRIVATE ${SIGNER_TEST_LIBS} nostr_gobject)
target_link_directories(test-social-recovery PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-social-recovery PRIVATE c_std_11)
apply_test_sanitizers(test-social-recovery)

add_test(NAME signer/social-recovery COMMAND test-social-recovery)
set_tests_properties(signer/social-recovery PROPERTIES
    TIMEOUT 60
    LABELS "signer;social-recovery;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# Test: Signer Core functionality (key generation, signing, NIP-44, event workflow)
# Comprehensive tests for core cryptographic and signing operations
# Issue: nostrc-ddh
# ==============================================================================
add_executable(test-signer-core test-signer-core.c)
target_include_directories(test-signer-core PRIVATE
    ${SIGNER_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/nips/nip44/include
)
target_link_libraries(test-signer-core PRIVATE ${SIGNER_TEST_LIBS})
# Link NIP-44 library for encryption tests
if(TARGET nostr_nip44_core)
    target_link_libraries(test-signer-core PRIVATE nostr_nip44_core)
endif()
target_link_directories(test-signer-core PRIVATE ${SIGNER_TEST_LINK_DIRS})
target_compile_features(test-signer-core PRIVATE c_std_11)
apply_test_sanitizers(test-signer-core)

add_test(NAME signer/signer-core COMMAND test-signer-core)
set_tests_properties(signer/signer-core PROPERTIES
    TIMEOUT 120
    LABELS "signer;core;crypto;signing;nip44;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto"
)

# ==============================================================================
# All signer tests target (convenience)
# ==============================================================================
add_custom_target(signer-tests
    DEPENDS test-crypto test-identity test-session test-dbus test-rate-limiter test-ui test-approval-dialog test-hsm-provider test-core test-secure-mem test-social-recovery test-signer-core
    COMMENT "Building all grotto tests"
)

# ==============================================================================
# Fuzz Testing Targets (optional, requires Clang and -DBUILD_FUZZ_TARGETS=ON)
# Issue: nostrc-p7f6
# ==============================================================================
add_subdirectory(fuzz)

# ==============================================================================
# Test: the profile guard (nostrc-wqn0u) - under test the real secret store,
# start-up and AccountsStore code refuse the owner's profile and key stores.
# ==============================================================================
if(TARGET grotto_bunker_core)
    add_executable(test-profile-guard test-profile-guard.c)
    target_include_directories(test-profile-guard PRIVATE
        ${SIGNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/tests/common)
    target_link_libraries(test-profile-guard PRIVATE grotto_bunker_core ${SIGNER_TEST_LIBS})
    target_link_directories(test-profile-guard PRIVATE ${SIGNER_TEST_LINK_DIRS})
    target_compile_features(test-profile-guard PRIVATE c_std_11)
    target_compile_options(test-profile-guard PRIVATE -Wall -Wextra -Werror)
    apply_test_sanitizers(test-profile-guard)
    add_test(NAME signer/profile-guard COMMAND test-profile-guard)
    set_tests_properties(signer/profile-guard PROPERTIES
        TIMEOUT 60
        LABELS "signer;security;unit")
endif()

# G05: real lifecycle/custody/configuration service coverage.
if(UNIX AND TARGET grotto_bunker_core)
    pkg_check_modules(G00_SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
    add_executable(test-bunker-startup test-bunker.c bunker_test_relay.c)
    target_link_libraries(test-bunker-startup PRIVATE grotto_bunker_core PkgConfig::G00_SOUP)
    target_compile_features(test-bunker-startup PRIVATE c_std_11)
    target_compile_options(test-bunker-startup PRIVATE -Wall -Wextra -Werror)
    apply_test_sanitizers(test-bunker-startup)
    add_dependencies(test-bunker-startup grotto-schemas)
    add_dependencies(signer-tests test-bunker-startup)
    add_test(NAME signer/bunker-startup COMMAND test-bunker-startup)
    add_test(NAME signer/bunker COMMAND test-bunker-startup)
    set_tests_properties(signer/bunker-startup signer/bunker PROPERTIES
        TIMEOUT 30 LABELS "signer;bunker;nip46;unit"
        ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto")
endif()

# G00 baseline: the real daemon over a signature-validating loopback relay.
if(UNIX AND TARGET grotto-daemon AND TARGET nostr_nip46_core)
    find_program(G00_GLIB_COMPILE_SCHEMAS glib-compile-schemas REQUIRED)
    pkg_check_modules(G00_SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
    add_executable(test-bunker-e2e test-bunker-e2e.c bunker_test_relay.c)
    target_include_directories(test-bunker-e2e PRIVATE ${SIGNER_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/apps/grotto/daemon ${CMAKE_SOURCE_DIR}/nips/nip55l/src/glib)
    target_link_libraries(test-bunker-e2e PRIVATE
        ${SIGNER_TEST_LIBS} nostr_nip46_core nostr_nip55l_core grotto_control_core PkgConfig::G00_SOUP nostrc-test-bus)
    target_link_directories(test-bunker-e2e PRIVATE ${SIGNER_TEST_LINK_DIRS})
    target_compile_features(test-bunker-e2e PRIVATE c_std_11)
    target_compile_options(test-bunker-e2e PRIVATE -Wall -Wextra -Werror)
    apply_test_sanitizers(test-bunker-e2e)
    add_dependencies(test-bunker-e2e grotto-daemon grotto-schemas)
    add_dependencies(signer-tests test-bunker-e2e)
    add_test(NAME signer/bunker-e2e COMMAND test-bunker-e2e
        $<TARGET_FILE:grotto-daemon>
        ${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Grotto.gschema.xml
        ${G00_GLIB_COMPILE_SCHEMAS})
    add_test(NAME signer/bunker-headless COMMAND test-bunker-e2e
        $<TARGET_FILE:grotto-daemon>
        ${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Grotto.gschema.xml
        ${G00_GLIB_COMPILE_SCHEMAS} --headless)
    # G05: normal success, no inverted nonfunctional baseline.
    set_tests_properties(signer/bunker-e2e signer/bunker-headless PROPERTIES
        TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "signer;bunker;nip46;integration")
endif()

# G04: real Control1 dispatcher, shared gate/evaluator, private message bus.
if (TARGET nostr_nip55l_glib)
  get_target_property(_nip55l_definitions nostr_nip55l_glib COMPILE_DEFINITIONS)
endif()
if (TARGET grotto_control_core AND "NIP55L_TEST_TRUST_ENV=1" IN_LIST _nip55l_definitions)
  add_executable(test-control-untrusted test-control-untrusted.c)
  target_link_libraries(test-control-untrusted PRIVATE PkgConfig::GLIB)
  foreach(suite clients activity)
    add_executable(test-${suite} test-${suite}.c)
    target_include_directories(test-${suite} PRIVATE ${CMAKE_SOURCE_DIR}
      ${CMAKE_SOURCE_DIR}/nips/nip55l/src/glib)
    target_compile_definitions(test-${suite} PRIVATE NIP55L_TEST_TRUST_ENV
      CONTROL_UNTRUSTED_BIN="$<TARGET_FILE:test-control-untrusted>"
      GROTTO_DAEMON_BIN="$<TARGET_FILE:grotto-daemon>")
    target_link_libraries(test-${suite} PRIVATE grotto_control_core nostrc-test-bus)
    if (TARGET nostr_nip49_glib)
      target_compile_definitions(test-${suite} PRIVATE GROTTO_HAVE_NIP49_EXPORT=1)
      target_link_libraries(test-${suite} PRIVATE nostr_nip49_glib)
    endif()
    add_dependencies(test-${suite} test-control-untrusted grotto-daemon)
    add_test(NAME signer/${suite} COMMAND test-${suite})
    set_tests_properties(signer/${suite} PROPERTIES
      TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "signer;control;integration"
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}/../data;GSETTINGS_BACKEND=memory")
  endforeach()
  add_dependencies(signer-tests test-clients test-activity)
endif()

# G07: production controller and dialog, with I/O mocked only at the bus.
add_library(grotto_approval_test_core STATIC
  ../src/approval-controller.c ../src/ui/approval_dialog.c)
target_include_directories(grotto_approval_test_core PUBLIC ${SIGNER_INCLUDE_DIRS})
target_link_libraries(grotto_approval_test_core PUBLIC PkgConfig::GTK4 PkgConfig::ADWAITA PkgConfig::GLIB)
target_compile_options(grotto_approval_test_core PRIVATE -Wall -Wextra -Werror)
add_executable(test-approval-controller test-approval-controller.c)
target_link_libraries(test-approval-controller PRIVATE grotto_approval_test_core nostrc-test-bus)
add_test(NAME signer/approval-controller COMMAND test-approval-controller)
set_tests_properties(signer/approval-controller PROPERTIES TIMEOUT 90 LABELS "signer;unit"
  ENVIRONMENT "${_UI_TEST_ENV}")
add_dependencies(signer-tests test-approval-controller)
if (TARGET grotto-daemon AND "NIP55L_TEST_TRUST_ENV=1" IN_LIST _nip55l_definitions)
  if(NOT APPLE)
    # Exercise the real main_app startup/actions and management page, not stubs.
    get_target_property(_g07_gui_sources grotto SOURCES)
    list(REMOVE_ITEM _g07_gui_sources "${APP_GRESOURCE_C}")
    add_library(grotto_app_test_core STATIC ${_g07_gui_sources})
    target_include_directories(grotto_app_test_core PRIVATE $<TARGET_PROPERTY:grotto,INCLUDE_DIRECTORIES>)
    target_compile_definitions(grotto_app_test_core PRIVATE GROTTO_APP_NO_MAIN=1
      $<TARGET_PROPERTY:grotto,COMPILE_DEFINITIONS>)
    target_link_directories(grotto_app_test_core PUBLIC $<TARGET_PROPERTY:grotto,LINK_DIRECTORIES>)
    target_link_libraries(grotto_app_test_core PUBLIC $<TARGET_PROPERTY:grotto,LINK_LIBRARIES>)
    set(_g07_integration_resource "${APP_GRESOURCE_C}")
  else()
    set(_g07_integration_resource "${_APPROVAL_GRESOURCE_C}")
  endif()
  add_executable(test-approval-integration test-approval-integration.c ${_g07_integration_resource})
  if(NOT APPLE)
    target_link_libraries(test-approval-integration PRIVATE grotto_app_test_core)
  endif()
  target_link_libraries(test-approval-integration PRIVATE grotto_approval_test_core nostrc-test-bus ${SIGNER_TEST_LIBS})
  target_link_directories(test-approval-integration PRIVATE ${SIGNER_TEST_LINK_DIRS})
  add_dependencies(test-approval-integration grotto-daemon grotto-schemas)
  add_test(NAME signer/approval-integration COMMAND test-approval-integration $<TARGET_FILE:grotto-daemon>)
  set_tests_properties(signer/approval-integration PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 LABELS "signer;integration"
    ENVIRONMENT "${_UI_TEST_ENV}")
  add_dependencies(signer-tests test-approval-integration)
endif()

# G08: real background state machine, fake portal only on the private bus.
if(UNIX)
  add_library(grotto_background_test_core STATIC ../src/background.c)
  target_include_directories(grotto_background_test_core PUBLIC ${SIGNER_INCLUDE_DIRS})
  target_link_libraries(grotto_background_test_core PUBLIC PkgConfig::GLIB)
  target_compile_definitions(grotto_background_test_core PRIVATE
    g_application_hold=g08_test_hold g_application_release=g08_test_release
    GROTTO_AUTOSTART_TEMPLATE="${CMAKE_BINARY_DIR}/apps/grotto/org.nostr.Grotto-autostart.desktop"
    GROTTO_EXECUTABLE="${CMAKE_INSTALL_FULL_BINDIR}/grotto")
  target_compile_options(grotto_background_test_core PRIVATE -Wall -Wextra -Werror)
  add_executable(test-background test-background.c)
  target_link_libraries(test-background PRIVATE grotto_background_test_core nostrc-test-bus)
  target_compile_definitions(test-background PRIVATE
    G08_EXECUTABLE="${CMAKE_INSTALL_FULL_BINDIR}/grotto"
    G08_TEMPLATE="${CMAKE_BINARY_DIR}/apps/grotto/org.nostr.Grotto-autostart.desktop")
  target_compile_options(test-background PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-background grotto-schemas)
  add_dependencies(signer-tests test-background)
  add_test(NAME signer/background COMMAND test-background)
  set_tests_properties(signer/background PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77
    LABELS "signer;unit;background"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto")
endif()
set(_g08_desktop_worker)
if(TARGET grotto_app_test_core)
  add_executable(test-desktop-activation test-desktop-activation.c ${APP_GRESOURCE_C})
  target_include_directories(test-desktop-activation PRIVATE ${SIGNER_INCLUDE_DIRS})
  target_link_libraries(test-desktop-activation PRIVATE grotto_app_test_core nostrc-test-bus)
  target_compile_options(test-desktop-activation PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-desktop-activation grotto-schemas)
  add_dependencies(signer-tests test-desktop-activation)
  set(_g08_desktop_worker $<TARGET_FILE:test-desktop-activation>)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME signer/desktop-contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test-desktop-contract.py ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} ${_g08_desktop_worker})
set_tests_properties(signer/desktop-contract PROPERTIES TIMEOUT 90 LABELS "signer;desktop;unit"
  ENVIRONMENT "${_UI_TEST_ENV}")

if(UNIX)
  add_executable(test-session-lock test-session-lock.c)
  target_include_directories(test-session-lock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../daemon
    ${CMAKE_SOURCE_DIR}/nips/nip55l/src/glib)
  target_link_libraries(test-session-lock PRIVATE grotto_control_core nostrc-test-bus)
  target_compile_options(test-session-lock PRIVATE -Wall -Wextra -Werror)
  add_dependencies(test-session-lock grotto-schemas)
  add_dependencies(signer-tests test-session-lock)
  add_test(NAME signer/session-lock COMMAND test-session-lock)
  set_tests_properties(signer/session-lock PROPERTIES TIMEOUT 60 LABELS "signer;security;unit"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/grotto")
endif()

# Private HOME/XDG_* and NOSTRC_TEST_ISOLATION for every test above
# (nostrc-wqn0u). Must stay last, after every add_test() in this directory.
if (BUILD_TESTING)
  include(${CMAKE_SOURCE_DIR}/cmake/NostrcTestIsolation.cmake)
  nostrc_isolate_tests(grotto-tests-test-homes)
endif()
