cmake_minimum_required(VERSION 3.10)
project(test_nostr_project)

# Unit/integration test binaries for nostrc. Uses CTest (enabled at the root).

enable_testing()

# Sanitizer helper (Debug builds)
function(apply_sanitizers target)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    if (GO_ENABLE_TSAN)
      target_compile_options(${target} PRIVATE -fsanitize=thread)
      target_link_options(${target} PRIVATE -fsanitize=thread)
    else()
      if (GO_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
      endif()
      if (GO_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
      endif()
    endif()
  endif()
endfunction()

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
add_executable(test_subscription_lifecycle test_subscription_lifecycle.c)
add_executable(test_envelope_parse test_envelope_parse.c)

# Link OpenSSL, libsecp256k1, nsync, and project libraries
target_link_libraries(test_nostr PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})

target_link_libraries(test_relay PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_relay_shutdown PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_subscription_lifecycle PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
target_link_libraries(test_envelope_parse PRIVATE libnostr ${NSYNC_LIB} nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})

# Apply sanitizers to tests if enabled
apply_sanitizers(test_nostr)
apply_sanitizers(test_relay)
apply_sanitizers(test_relay_shutdown)
apply_sanitizers(test_subscription_lifecycle)
apply_sanitizers(test_envelope_parse)

# Register tests for CTest. Run with: `ctest --output-on-failure`
add_test(NAME test_nostr COMMAND test_nostr)
add_test(NAME test_relay COMMAND test_relay)
add_test(NAME test_relay_shutdown COMMAND test_relay_shutdown)
add_test(NAME test_subscription_lifecycle COMMAND test_subscription_lifecycle)
add_test(NAME test_envelope_parse COMMAND test_envelope_parse)

# Include directories for libsecp256k1 headers
target_include_directories(test_nostr PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_relay_shutdown PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_subscription_lifecycle PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_include_directories(test_envelope_parse PRIVATE ${SECP256K1_INCLUDE_DIRS})

# Link directories for libsecp256k1
target_link_directories(test_nostr PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_relay_shutdown PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_subscription_lifecycle PRIVATE ${SECP256K1_LIBRARY_DIRS})
target_link_directories(test_envelope_parse PRIVATE ${SECP256K1_LIBRARY_DIRS})
