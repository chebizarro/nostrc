project(nip25)

# Sources
file(GLOB SOURCES "src/*.c")

# Library
add_library(nip25 ${SOURCES})
target_link_libraries(nip25 nostr)
target_include_directories(nip25
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

# Install headers and library when this NIP is enabled
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)
install(TARGETS nip25
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip25 ${TEST_SOURCES})
    target_include_directories(test_nip25 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip25 nip25 nostr)
    add_test(NAME test_nip25 COMMAND test_nip25)
endif()

# Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip25_example examples/example.c)
    target_include_directories(nip25_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(nip25_example nip25 nostr)
endif()
