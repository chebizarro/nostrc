cmake_minimum_required(VERSION 3.16)
project(nip98)

set(NIP98_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP98_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nip98.c
)

# Create the library
add_library(nostr_nip98 ${NIP98_SRC})

# Include directories
target_include_directories(nostr_nip98 PUBLIC
    ${NIP98_INC}
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
)

# Link dependencies
find_package(OpenSSL REQUIRED)
target_link_libraries(nostr_nip98 PUBLIC nostr OpenSSL::Crypto)

# Install headers
install(DIRECTORY ${NIP98_INC}/nostr/nip98/ DESTINATION include/nostr/nip98)

# Tests
enable_testing()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c")
    foreach(TEST_SRC ${TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        add_executable(${TEST_NAME} ${TEST_SRC})
        target_include_directories(${TEST_NAME} PRIVATE ${NIP98_INC})
        target_link_libraries(${TEST_NAME} PRIVATE nostr nostr_nip98 OpenSSL::Crypto)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()
endif()
