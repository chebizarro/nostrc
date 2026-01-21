# testing/CMakeLists.txt
#
# Build the nostr_testing library - provides test harness utilities,
# event factories, deterministic keypairs, assertion macros, and mock relay server.

cmake_minimum_required(VERSION 3.30)
project(nostr_testing C)

# Find required packages
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

# Find nsync
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Test harness library sources
set(NOSTR_TESTING_SOURCES
    src/test_harness.c
    src/mock_relay.c
    src/mock_relay_server.c
)

# Create static library
add_library(nostr_testing STATIC ${NOSTR_TESTING_SOURCES})

# Include directories
target_include_directories(nostr_testing
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/libgo/include
        ${LWS_INCLUDE_DIRS}
    PRIVATE
        ${NSYNC_INCLUDE_DIR}
        ${SECP256K1_INCLUDE_DIRS}
)

# Link dependencies
target_link_libraries(nostr_testing
    PUBLIC
        libnostr
        nostr_json
        libgo
        ${NSYNC_LIB}
        ${LWS_LIBRARIES}
        PkgConfig::JANSSON
        pthread
    PRIVATE
        OpenSSL::Crypto
        ${SECP256K1_LIBRARIES}
)

# Link directories for secp256k1 and libwebsockets
target_link_directories(nostr_testing PRIVATE
    ${SECP256K1_LIBRARY_DIRS}
    ${LWS_LIBRARY_DIRS}
)

# Compile definitions
target_compile_definitions(nostr_testing PRIVATE
    NOSTR_TEST_FIXTURES_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# Export variables for use by tests in other directories
set(NOSTR_TESTING_LIBRARY nostr_testing PARENT_SCOPE)
set(NOSTR_TESTING_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include PARENT_SCOPE)
set(NOSTR_TEST_FIXTURES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures PARENT_SCOPE)

# Sanitizer support (inherit from root)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(nostr_testing PRIVATE -fsanitize=thread)
        target_link_options(nostr_testing PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(nostr_testing PRIVATE -fsanitize=address)
            target_link_options(nostr_testing PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(nostr_testing PRIVATE -fsanitize=undefined)
            target_link_options(nostr_testing PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

# Compiler warnings
target_compile_options(nostr_testing PRIVATE -Wall -Wextra)

# ============================================================================
# Example Test Executable
# ============================================================================

add_executable(test_harness_example tests/test_harness_example.c)
target_link_libraries(test_harness_example PRIVATE
    nostr_testing
    libnostr
    nostr_json
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
)
target_include_directories(test_harness_example PRIVATE
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
)
target_link_directories(test_harness_example PRIVATE ${SECP256K1_LIBRARY_DIRS})

# Apply sanitizers to example test
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(test_harness_example PRIVATE -fsanitize=thread)
        target_link_options(test_harness_example PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(test_harness_example PRIVATE -fsanitize=address)
            target_link_options(test_harness_example PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(test_harness_example PRIVATE -fsanitize=undefined)
            target_link_options(test_harness_example PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

# Register test with CTest
add_test(NAME TestHarnessExample COMMAND test_harness_example)
set_tests_properties(TestHarnessExample PROPERTIES
    TIMEOUT 30
    LABELS "harness;unit"
    ENVIRONMENT "NOSTR_TEST_FIXTURES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

# ============================================================================
# Mock Relay CLI Tool
# ============================================================================

add_executable(mock-relay tools/mock-relay-main.c)
target_link_libraries(mock-relay PRIVATE
    nostr_testing
    libnostr
    nostr_json
    ${NSYNC_LIB}
    ${LWS_LIBRARIES}
    PkgConfig::JANSSON
    pthread
)
target_include_directories(mock-relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NSYNC_INCLUDE_DIR}
    ${LWS_INCLUDE_DIRS}
)
target_link_directories(mock-relay PRIVATE ${LWS_LIBRARY_DIRS})

# Apply sanitizers to mock-relay
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(mock-relay PRIVATE -fsanitize=thread)
        target_link_options(mock-relay PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(mock-relay PRIVATE -fsanitize=address)
            target_link_options(mock-relay PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(mock-relay PRIVATE -fsanitize=undefined)
            target_link_options(mock-relay PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

# ============================================================================
# Mock Relay Server Tests
# ============================================================================

# Test: Mock relay server basic operations
add_executable(test_mock_relay_server tests/test_mock_relay_server.c)
target_link_libraries(test_mock_relay_server PRIVATE
    nostr_testing
    libnostr
    nostr_json
    ${NSYNC_LIB}
    ${LWS_LIBRARIES}
    PkgConfig::JANSSON
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
    pthread
)
target_include_directories(test_mock_relay_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
    ${LWS_INCLUDE_DIRS}
)
target_link_directories(test_mock_relay_server PRIVATE
    ${SECP256K1_LIBRARY_DIRS}
    ${LWS_LIBRARY_DIRS}
)

# Apply sanitizers to mock relay server test
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(test_mock_relay_server PRIVATE -fsanitize=thread)
        target_link_options(test_mock_relay_server PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(test_mock_relay_server PRIVATE -fsanitize=address)
            target_link_options(test_mock_relay_server PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(test_mock_relay_server PRIVATE -fsanitize=undefined)
            target_link_options(test_mock_relay_server PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

add_test(NAME MockRelayServerBasic COMMAND test_mock_relay_server)
set_tests_properties(MockRelayServerBasic PROPERTIES
    TIMEOUT 60
    LABELS "mock;integration"
)

# Test: Mock relay integration test (with real client)
add_executable(test_mock_relay_integration tests/test_mock_relay_integration.c)
target_link_libraries(test_mock_relay_integration PRIVATE
    nostr_testing
    libnostr
    nostr_json
    ${NSYNC_LIB}
    ${LWS_LIBRARIES}
    PkgConfig::JANSSON
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
    pthread
)
target_include_directories(test_mock_relay_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
    ${LWS_INCLUDE_DIRS}
)
target_link_directories(test_mock_relay_integration PRIVATE
    ${SECP256K1_LIBRARY_DIRS}
    ${LWS_LIBRARY_DIRS}
)

# Apply sanitizers to integration test
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(test_mock_relay_integration PRIVATE -fsanitize=thread)
        target_link_options(test_mock_relay_integration PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(test_mock_relay_integration PRIVATE -fsanitize=address)
            target_link_options(test_mock_relay_integration PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(test_mock_relay_integration PRIVATE -fsanitize=undefined)
            target_link_options(test_mock_relay_integration PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

add_test(NAME MockRelayIntegration COMMAND test_mock_relay_integration)
set_tests_properties(MockRelayIntegration PROPERTIES
    TIMEOUT 90
    LABELS "mock;integration"
)

# Test: In-process mock relay unit tests
add_executable(test_mock_relay tests/test_mock_relay.c)
target_link_libraries(test_mock_relay PRIVATE
    nostr_testing
    libnostr
    nostr_json
    libgo
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
    pthread
)
target_include_directories(test_mock_relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
)
target_link_directories(test_mock_relay PRIVATE ${SECP256K1_LIBRARY_DIRS})

add_test(NAME MockRelayUnit COMMAND test_mock_relay)
set_tests_properties(MockRelayUnit PROPERTIES
    TIMEOUT 30
    LABELS "mock;unit"
)

# ============================================================================
# Signer Integration Tests (NIP-55L)
# ============================================================================

# Note: These tests link against NIP libraries defined in ../nips/.
# The libraries are built before this test due to cmake's dependency resolution.

add_executable(test_signer_integration tests/test_signer_integration.c)
target_link_libraries(test_signer_integration PRIVATE
    nostr_nip55l_core
    libnostr
    nostr_json
    nip19
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
)
target_include_directories(test_signer_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/nips/nip55l/include
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
)
target_link_directories(test_signer_integration PRIVATE ${SECP256K1_LIBRARY_DIRS})

# Apply sanitizers
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(test_signer_integration PRIVATE -fsanitize=thread)
        target_link_options(test_signer_integration PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(test_signer_integration PRIVATE -fsanitize=address)
            target_link_options(test_signer_integration PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(test_signer_integration PRIVATE -fsanitize=undefined)
            target_link_options(test_signer_integration PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

add_test(NAME SignerIntegration COMMAND test_signer_integration)
set_tests_properties(SignerIntegration PROPERTIES
    TIMEOUT 60
    LABELS "signer;integration"
)

# ============================================================================
# NIP-46 Remote Signing Tests
# ============================================================================

add_executable(test_nip46_mock_relay tests/test_nip46_mock_relay.c)
target_link_libraries(test_nip46_mock_relay PRIVATE
    nostr_nip46_core
    nostr_testing
    libnostr
    nostr_json
    ${NSYNC_LIB}
    ${LWS_LIBRARIES}
    PkgConfig::JANSSON
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIBRARIES}
    pthread
)
target_include_directories(test_nip46_mock_relay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${NSYNC_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIRS}
    ${LWS_INCLUDE_DIRS}
)
target_link_directories(test_nip46_mock_relay PRIVATE
    ${SECP256K1_LIBRARY_DIRS}
    ${LWS_LIBRARY_DIRS}
)

# Apply sanitizers
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(GO_ENABLE_TSAN)
        target_compile_options(test_nip46_mock_relay PRIVATE -fsanitize=thread)
        target_link_options(test_nip46_mock_relay PRIVATE -fsanitize=thread)
    else()
        if(GO_ENABLE_ASAN)
            target_compile_options(test_nip46_mock_relay PRIVATE -fsanitize=address)
            target_link_options(test_nip46_mock_relay PRIVATE -fsanitize=address)
        endif()
        if(GO_ENABLE_UBSAN)
            target_compile_options(test_nip46_mock_relay PRIVATE -fsanitize=undefined)
            target_link_options(test_nip46_mock_relay PRIVATE -fsanitize=undefined)
        endif()
    endif()
endif()

add_test(NAME Nip46MockRelay COMMAND test_nip46_mock_relay)
set_tests_properties(Nip46MockRelay PROPERTIES
    TIMEOUT 60
    LABELS "nip46;signer;integration"
)

# ============================================================================
# Install Rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS nostr_testing
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nostr/testing
)
