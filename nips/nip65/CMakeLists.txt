project(nip65)

add_library(nip65 OBJECT
    src/nip65.c)

target_include_directories(nip65
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip65 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip65 ${TEST_SOURCES})
    target_include_directories(test_nip65 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip65 nip65 nostr)
    add_test(NAME test_nip65 COMMAND test_nip65)
endif()

# Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip65_example examples/example.c)
    target_include_directories(nip65_example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(nip65_example nip65 nostr)
endif()
