cmake_minimum_required(VERSION 3.10)
project(test_nostr_project)

enable_testing()

# Find OpenSSL
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

# Add the executable
add_executable(test_nostr tests_nostr.c)

# Link OpenSSL and libsecp256k1
target_link_libraries(test_nostr libnostr nostr_json OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})

# Add test
add_test(NAME test_nostr COMMAND test_nostr)

# Include directories for libsecp256k1
target_include_directories(test_nostr PRIVATE ${SECP256K1_INCLUDE_DIRS})
