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
    ENVIRONMENT "GSETTINGS_BACKEND=memory"
)

# ==============================================================================
# All signer tests target (convenience)
# ==============================================================================
add_custom_target(signer-tests
    DEPENDS test-crypto test-identity test-session
    COMMENT "Building all gnostr-signer tests"
)
