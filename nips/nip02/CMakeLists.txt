project(nip02)

add_library(nip02 OBJECT
    src/nip02.c)

target_include_directories(nip02
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip02 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip02 ${TEST_SOURCES})
    target_include_directories(test_nip02 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip02 nip02 nostr)
    add_test(NAME test_nip02 COMMAND test_nip02)
endif()

# Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip02_example examples/example.c)
    target_include_directories(nip02_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(nip02_example nip02 nostr)
endif()
