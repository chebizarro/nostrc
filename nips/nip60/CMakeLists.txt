include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip60)

add_library(nip60 OBJECT
    src/nip60.c)
nostrc_library_visibility(nip60 NOSTR_NIP60_API
  nostr/nip60/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip60")

target_include_directories(nip60
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip60 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip60 ${TEST_SOURCES})
    target_include_directories(test_nip60 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip60 nip60 nostr)
    add_test(NAME test_nip60 COMMAND test_nip60)
endif()
