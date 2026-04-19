project(nip24)

add_library(nip24 OBJECT
    src/nip24.c)

target_include_directories(nip24
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip24 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip24 ${TEST_SOURCES})
    target_include_directories(test_nip24 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip24 nip24 nostr)
    add_test(NAME test_nip24 COMMAND test_nip24)
endif()
