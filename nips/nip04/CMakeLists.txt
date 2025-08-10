project(nip04)

# Include the header files
include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_BINARY_DIR}/libnostr/generated
)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Dependencies
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)

# Create the library
add_library(nip04 ${SOURCES})
target_link_libraries(nip04 PUBLIC nostr OpenSSL::Crypto ${SECP256K1_LIB})

# Example
add_executable(nip04_example examples/examples.c)
target_link_libraries(nip04_example PRIVATE nip04 nostr OpenSSL::Crypto ${SECP256K1_LIB})

# Install rules
install(TARGETS nip04
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(FILES include/nostr/nip04.h DESTINATION include/nostr)

# Add tests if available
# Per-test executables (avoid duplicate main)
file(GLOB TEST_SOURCES "tests/*.c")
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE nip04 nostr OpenSSL::Crypto ${SECP256K1_LIB})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
