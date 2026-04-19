project(nip28)

add_library(nip28 OBJECT
    src/nip28.c)

target_include_directories(nip28
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip28 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip28 ${TEST_SOURCES})
    target_include_directories(test_nip28 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip28 nip28 nostr)
    add_test(NAME test_nip28 COMMAND test_nip28)
endif()
