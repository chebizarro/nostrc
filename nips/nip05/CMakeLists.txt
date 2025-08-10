
project(nip05)

# Include the header files
include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_BINARY_DIR}/libnostr/generated
)

# Dependencies
find_package(CURL REQUIRED)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip05 ${SOURCES})
target_link_libraries(nip05 PUBLIC nostr CURL::libcurl)

# Example
add_executable(nip05_example examples/example.c)
target_link_libraries(nip05_example PRIVATE nip05 nostr nostr_json CURL::libcurl)

# Install rules
install(TARGETS nip05
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
install(FILES include/nip05.h DESTINATION include/nostr)

# Per-test executables (avoid duplicate main)
file(GLOB TEST_SOURCES "tests/*.c")
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} PRIVATE nip05 nostr nostr_json CURL::libcurl)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

