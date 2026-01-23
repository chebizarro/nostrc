
project(nip18)

# Sources
file(GLOB SOURCES "src/*.c")

# Library
add_library(nip18 ${SOURCES})
target_link_libraries(nip18 nostr)
target_include_directories(nip18
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

# Install headers and library when this NIP is enabled
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)
install(TARGETS nip18
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip18 ${TEST_SOURCES})
    target_include_directories(test_nip18 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip18 nip18 nostr)
    add_test(NAME test_nip18 COMMAND test_nip18)
endif()

# Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip18_example examples/example.c)
    target_include_directories(nip18_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(nip18_example nip18 nostr)
endif()

