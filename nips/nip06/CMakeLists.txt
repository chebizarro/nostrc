
project(nip06)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Add wally
find_path(WALLY_INCLUDE_DIR wally_core.h HINTS $ENV{HOME}/.local/include /usr/local/include /opt/homebrew/include)
find_library(WALLY_CORE_LIB wallycore HINTS $ENV{HOME}/.local/lib /usr/local/lib /opt/homebrew/lib)
include_directories(${WALLY_INCLUDE_DIR})

# Create the library
add_library(nip06 ${SOURCES})
target_link_libraries(nip06 nostr ${WALLY_CORE_LIB})

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip06 ${TEST_SOURCES} ${WALLY_CORE_LIB})
    target_link_libraries(test_nip06 nip06 nostr ${WALLY_CORE_LIB})
    add_test(NAME test_nip06 COMMAND test_nip06)
endif()

