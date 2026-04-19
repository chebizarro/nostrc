project(nip99)

add_library(nip99 OBJECT
    src/nip99.c)

target_include_directories(nip99
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip99 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip99 ${TEST_SOURCES})
    target_include_directories(test_nip99 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip99 nip99 nostr)
    add_test(NAME test_nip99 COMMAND test_nip99)
endif()
