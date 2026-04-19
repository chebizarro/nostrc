project(nip14)

add_library(nip14 OBJECT
    src/nip14.c)

target_include_directories(nip14
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip14 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip14 ${TEST_SOURCES})
    target_include_directories(test_nip14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip14 nip14 nostr)
    add_test(NAME test_nip14 COMMAND test_nip14)
endif()
