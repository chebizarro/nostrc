project(nipb0)

add_library(nipb0 OBJECT
    src/nipb0.c)

target_include_directories(nipb0
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nipb0 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nipb0 ${TEST_SOURCES})
    target_include_directories(test_nipb0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nipb0 nipb0 nostr)
    add_test(NAME test_nipb0 COMMAND test_nipb0)
endif()
