project(nip61)

add_library(nip61 OBJECT
    src/nip61.c)

target_include_directories(nip61
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip61 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip61 ${TEST_SOURCES})
    target_include_directories(test_nip61 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip61 nip61 nostr)
    add_test(NAME test_nip61 COMMAND test_nip61)
endif()
