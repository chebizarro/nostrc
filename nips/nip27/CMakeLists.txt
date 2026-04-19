project(nip27)

add_library(nip27 OBJECT
    src/nip27.c)

target_include_directories(nip27
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip19/include
        ${CMAKE_SOURCE_DIR}/nips/nip21/include)

target_link_libraries(nip27 PUBLIC nostr nip19 nip21)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip27 ${TEST_SOURCES})
    target_include_directories(test_nip27 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip19/include
        ${CMAKE_SOURCE_DIR}/nips/nip21/include)
    target_link_libraries(test_nip27 nip27 nip21 nip19 nostr)
    add_test(NAME test_nip27 COMMAND test_nip27)
endif()
