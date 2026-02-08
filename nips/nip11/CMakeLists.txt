
project(nip11)

# Include the header files
include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

# Dependencies
find_package(CURL REQUIRED)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(nip11 ${SOURCES})
target_link_libraries(nip11 PUBLIC nostr nostr_json CURL::libcurl)

# Example (if present)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
  add_executable(nip11_example examples/example.c)
  target_link_libraries(nip11_example PRIVATE nip11 nostr CURL::libcurl)
endif()

# Add tests if available
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip11.c)
    add_executable(test_nip11 tests/test_nip11.c)
    target_link_libraries(test_nip11 PRIVATE nip11 nostr CURL::libcurl)
    add_test(NAME test_nip11 COMMAND test_nip11)
endif()

