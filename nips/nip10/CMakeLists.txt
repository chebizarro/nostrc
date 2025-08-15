
project(nip10)

# Sources
file(GLOB SOURCES "src/*.c")

# Library
add_library(nip10 ${SOURCES})
target_link_libraries(nip10 nostr)
target_include_directories(nip10
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

# Install headers and library when this NIP is enabled
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)
install(TARGETS nip10
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip10 ${TEST_SOURCES})
    target_include_directories(test_nip10 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip10 nip10 nostr)
    add_test(NAME test_nip10 COMMAND test_nip10)
endif()

# Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip10_example examples/example.c)
    target_include_directories(nip10_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(nip10_example nip10 nostr)
endif()

