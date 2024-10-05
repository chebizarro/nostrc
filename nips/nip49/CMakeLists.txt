
project(nip49)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip49 ${SOURCES})
target_link_libraries(nip49 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip49 ${TEST_SOURCES})
    target_link_libraries(test_nip49 nip49 nostr)
    add_test(NAME test_nip49 COMMAND test_nip49)
endif()

