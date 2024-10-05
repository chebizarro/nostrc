
project(nip11)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip11 ${SOURCES})
target_link_libraries(nip11 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip11 ${TEST_SOURCES})
    target_link_libraries(test_nip11 nip11 nostr)
    add_test(NAME test_nip11 COMMAND test_nip11)
endif()

