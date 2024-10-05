
project(nip42)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip42 ${SOURCES})
target_link_libraries(nip42 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip42 ${TEST_SOURCES})
    target_link_libraries(test_nip42 nip42 nostr)
    add_test(NAME test_nip42 COMMAND test_nip42)
endif()

