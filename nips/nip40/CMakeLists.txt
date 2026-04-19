project(nip40)

add_library(nip40 OBJECT
    src/nip40.c)

target_include_directories(nip40
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip40 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip40 ${TEST_SOURCES})
    target_include_directories(test_nip40 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip40 nip40 nostr)
    add_test(NAME test_nip40 COMMAND test_nip40)
endif()
