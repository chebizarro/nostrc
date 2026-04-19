project(nip39)

add_library(nip39 OBJECT
    src/nip39.c)

target_include_directories(nip39
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip39 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip39 ${TEST_SOURCES})
    target_include_directories(test_nip39 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip39 nip39 nostr)
    add_test(NAME test_nip39 COMMAND test_nip39)
endif()
