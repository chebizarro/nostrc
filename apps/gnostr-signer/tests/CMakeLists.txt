# gnostr-signer unit tests
# Uses GLib testing framework for consistency with GTK application

cmake_minimum_required(VERSION 3.30)
project(gnostr-signer-tests C)

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
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/nips/nip04/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
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

# Add NIP libraries if available
if(TARGET nip19)
    list(APPEND SIGNER_TEST_LIBS nip19)
endif()
if(TARGET nostr_nip49_core)
    list(APPEND SIGNER_TEST_LIBS nostr_nip49_core)
endif()
if(TARGET nip04)
    list(APPEND SIGNER_TEST_LIBS nip04)
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
target_link_directories(test-crypto PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-crypto PRIVATE c_std_11)
apply_test_sanitizers(test-crypto)

add_test(NAME signer/crypto COMMAND test-crypto)
set_tests_properties(signer/crypto PROPERTIES
    TIMEOUT 60
    LABELS "signer;crypto;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: Identity/Account store operations
# ==============================================================================
add_executable(test-identity test-identity.c)
target_include_directories(test-identity PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-identity PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-identity PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-identity PRIVATE c_std_11)
apply_test_sanitizers(test-identity)

add_test(NAME signer/identity COMMAND test-identity)
set_tests_properties(signer/identity PROPERTIES
    TIMEOUT 30
    LABELS "signer;identity;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: Session/Settings manager
# ==============================================================================
add_executable(test-session test-session.c)
target_include_directories(test-session PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-session PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-session PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-session PRIVATE c_std_11)
apply_test_sanitizers(test-session)

add_test(NAME signer/session COMMAND test-session)
set_tests_properties(signer/session PROPERTIES
    TIMEOUT 30
    LABELS "signer;session;unit"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: NIP-46 Bunker service (remote signing)
# ==============================================================================
add_executable(test-bunker test-bunker.c)
target_include_directories(test-bunker PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-bunker PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-bunker PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-bunker PRIVATE c_std_11)
apply_test_sanitizers(test-bunker)

add_test(NAME signer/bunker COMMAND test-bunker)
set_tests_properties(signer/bunker PROPERTIES
    TIMEOUT 30
    LABELS "signer;bunker;nip46;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: D-Bus interface integration tests
# Uses GTestDBus for isolated session bus testing
# Issue: nostrc-991
# ==============================================================================
add_executable(test-dbus test-dbus.c)
target_include_directories(test-dbus PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-dbus PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-dbus PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-dbus PRIVATE c_std_11)
apply_test_sanitizers(test-dbus)

add_test(NAME signer/dbus COMMAND test-dbus)
set_tests_properties(signer/dbus PROPERTIES
    TIMEOUT 120
    LABELS "signer;dbus;integration"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;G_DEBUG=fatal-warnings;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: Rate limiter (brute force attack prevention)
# Issue: nostrc-1g1
# ==============================================================================
add_executable(test-rate-limiter
    test-rate-limiter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rate-limiter.c
)
target_include_directories(test-rate-limiter PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-rate-limiter PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-rate-limiter PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-rate-limiter PRIVATE c_std_11)
apply_test_sanitizers(test-rate-limiter)

add_test(NAME signer/rate-limiter COMMAND test-rate-limiter)
set_tests_properties(signer/rate-limiter PROPERTIES
    TIMEOUT 30
    LABELS "signer;rate-limiter;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: UI components (window, dialogs, navigation, form validation)
# Uses GTK test utilities with mock widgets to avoid D-Bus/network dependencies
# ==============================================================================
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

add_executable(test-ui test-ui.c)
target_include_directories(test-ui PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-ui PRIVATE
    ${SIGNER_TEST_LIBS}
    PkgConfig::GTK4
    PkgConfig::ADWAITA
)
target_link_directories(test-ui PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-ui PRIVATE c_std_11)
apply_test_sanitizers(test-ui)

add_test(NAME signer/ui COMMAND test-ui)
set_tests_properties(signer/ui PROPERTIES
    TIMEOUT 60
    LABELS "signer;ui;unit"
    ENVIRONMENT "GDK_BACKEND=x11;GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer;GTK_A11Y=none"
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
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
)
target_link_libraries(test-hsm-provider PRIVATE
    ${SIGNER_TEST_LIBS}
)
# Link nip19 for npub encoding
if(TARGET nip19)
    target_link_libraries(test-hsm-provider PRIVATE nip19)
endif()
target_link_directories(test-hsm-provider PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-hsm-provider PRIVATE c_std_11)
apply_test_sanitizers(test-hsm-provider)

add_test(NAME signer/hsm-provider COMMAND test-hsm-provider)
set_tests_properties(signer/hsm-provider PROPERTIES
    TIMEOUT 30
    LABELS "signer;hsm;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# Test: Core signer modules (secret_store, backup-recovery, session, relay, delegation)
# Uses mock implementations to test core functionality without external dependencies
# Issue: nostrc-ddh
# ==============================================================================
add_executable(test-core test-core.c)
target_include_directories(test-core PRIVATE ${SIGNER_INCLUDE_DIRS})
target_link_libraries(test-core PRIVATE ${SIGNER_TEST_LIBS})
target_link_directories(test-core PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-core PRIVATE c_std_11)
apply_test_sanitizers(test-core)

add_test(NAME signer/core COMMAND test-core)
set_tests_properties(signer/core PROPERTIES
    TIMEOUT 60
    LABELS "signer;core;unit;secret_store;backup;session;relay;delegation"
    ENVIRONMENT "GSETTINGS_BACKEND=memory;GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
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
target_link_directories(test-secure-mem PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_compile_features(test-secure-mem PRIVATE c_std_11)
apply_test_sanitizers(test-secure-mem)

add_test(NAME signer/secure-mem COMMAND test-secure-mem)
set_tests_properties(signer/secure-mem PROPERTIES
    TIMEOUT 30
    LABELS "signer;secure-mem;security;unit"
    ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_BINARY_DIR}/apps/gnostr-signer"
)

# ==============================================================================
# All signer tests target (convenience)
# ==============================================================================
add_custom_target(signer-tests
    DEPENDS test-crypto test-identity test-session test-bunker test-dbus test-rate-limiter test-ui test-hsm-provider test-core test-secure-mem
    COMMENT "Building all gnostr-signer tests"
)

# ==============================================================================
# Fuzz Testing Targets (optional, requires Clang and -DBUILD_FUZZ_TARGETS=ON)
# Issue: nostrc-p7f6
# ==============================================================================
add_subdirectory(fuzz)
