project(nip22)

add_library(nip22 OBJECT
    src/nip22.c)

target_include_directories(nip22
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip22 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip22 ${TEST_SOURCES})
    target_include_directories(test_nip22 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip22 nip22 nostr)
    add_test(NAME test_nip22 COMMAND test_nip22)
endif()
