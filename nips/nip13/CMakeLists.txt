
project(nip13)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip13 ${SOURCES})
target_link_libraries(nip13 nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip13 ${TEST_SOURCES})
    target_link_libraries(test_nip13 nip13 nostr)
    add_test(NAME test_nip13 COMMAND test_nip13)
endif()

