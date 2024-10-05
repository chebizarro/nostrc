
project(nip53)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip53 ${SOURCES})
target_link_libraries(nip53 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip53 ${TEST_SOURCES})
    target_link_libraries(test_nip53 nip53 nostr)
    add_test(NAME test_nip53 COMMAND test_nip53)
endif()

