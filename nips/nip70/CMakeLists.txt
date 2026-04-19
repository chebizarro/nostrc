project(nip70)

add_library(nip70 OBJECT
    src/nip70.c)

target_include_directories(nip70
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip70 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip70 ${TEST_SOURCES})
    target_include_directories(test_nip70 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip70 nip70 nostr)
    add_test(NAME test_nip70 COMMAND test_nip70)
endif()
