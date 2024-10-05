
project(nip46)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip46 ${SOURCES})
target_link_libraries(nip46 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip46 ${TEST_SOURCES})
    target_link_libraries(test_nip46 nip46 nostr)
    add_test(NAME test_nip46 COMMAND test_nip46)
endif()

