cmake_minimum_required(VERSION 3.10)
project(test_nostr_project)

# Unit/integration test binaries for nostrc. Uses CTest (enabled at the root).

enable_testing()

# Find OpenSSL for hashing/crypto used by tests
find_package(OpenSSL REQUIRED)
if (OpenSSL_FOUND)
    message(STATUS "Found OpenSSL version ${OpenSSL_VERSION}")
else()
    message(FATAL_ERROR "OpenSSL not found")
endif()

# Find secp256k1 (assuming pkg-config is available)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
if (SECP256K1_FOUND)
    message(STATUS "Found secp256k1 version ${SECP256K1_VERSION}")
else()
    message(FATAL_ERROR "libsecp256k1 not found")
endif()

# Find the nsync library and headers manually for synchronization primitives
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})


# Test executables
add_executable(test_nostr tests_nostr.c)

add_executable(test_relay test_relay.c)
add_executable(test_relay_shutdown test_relay_shutdown.c)

# Link OpenSSL, libsecp256k1, nsync, and project libraries
target_link_libraries(test_nostr PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})

target_link_libraries(test_relay PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_relay_shutdown PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})

# Register tests for CTest. Run with: `ctest --output-on-failure`
add_test(NAME test_nostr COMMAND test_nostr)
add_test(NAME test_relay COMMAND test_relay)
add_test(NAME test_relay_shutdown COMMAND test_relay_shutdown)

# Include directories for libsecp256k1 headers
target_include_directories(test_nostr PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay_shutdown PRIVATE ${SECP256K1_INCLUDE_DIRS})

# Link directories for libsecp256k1
target_link_directories(test_nostr PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay_shutdown PRIVATE ${SECP256K1_LIBRARY_DIRS})
