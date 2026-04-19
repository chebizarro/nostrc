project(nip58)

add_library(nip58 OBJECT
    src/nip58.c)

target_include_directories(nip58
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip58 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip58 ${TEST_SOURCES})
    target_include_directories(test_nip58 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip58 nip58 nostr)
    add_test(NAME test_nip58 COMMAND test_nip58)
endif()
