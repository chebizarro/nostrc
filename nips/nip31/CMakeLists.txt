
project(nip31)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip31 ${SOURCES})
target_link_libraries(nip31 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip31 ${TEST_SOURCES})
    target_link_libraries(test_nip31 nip31 nostr)
    add_test(NAME test_nip31 COMMAND test_nip31)
endif()

