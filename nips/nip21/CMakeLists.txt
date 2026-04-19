project(nip21)

add_library(nip21 OBJECT
    src/nip21.c)

target_include_directories(nip21
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip19/include)

target_link_libraries(nip21 PUBLIC nostr nip19)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip21 ${TEST_SOURCES})
    target_include_directories(test_nip21 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip19/include)
    target_link_libraries(test_nip21 nip21 nip19 nostr)
    add_test(NAME test_nip21 COMMAND test_nip21)
endif()
