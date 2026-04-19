project(nip75)

add_library(nip75 OBJECT
    src/nip75.c)

target_include_directories(nip75
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip75 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip75 ${TEST_SOURCES})
    target_include_directories(test_nip75 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip75 nip75 nostr m)
    add_test(NAME test_nip75 COMMAND test_nip75)
endif()
