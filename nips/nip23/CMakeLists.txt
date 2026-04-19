project(nip23)

add_library(nip23 OBJECT
    src/nip23.c)

target_include_directories(nip23
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip23 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip23 ${TEST_SOURCES})
    target_include_directories(test_nip23 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip23 nip23 nostr)
    add_test(NAME test_nip23 COMMAND test_nip23)
endif()
