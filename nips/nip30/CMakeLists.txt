project(nip30)

add_library(nip30 OBJECT
    src/nip30.c)

target_include_directories(nip30
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip30 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip30 ${TEST_SOURCES})
    target_include_directories(test_nip30 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip30 nip30 nostr)
    add_test(NAME test_nip30 COMMAND test_nip30)
endif()
