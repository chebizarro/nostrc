project(nip73)

add_library(nip73 OBJECT
    src/nip73.c)

target_include_directories(nip73
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip73 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip73 ${TEST_SOURCES})
    target_include_directories(test_nip73 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip73 nip73 nostr)
    add_test(NAME test_nip73 COMMAND test_nip73)
endif()
