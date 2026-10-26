include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip92)

add_library(nip92 OBJECT
    src/nip92.c)
nostrc_library_visibility(nip92 NOSTR_NIP92_API
  nostr/nip92/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip92")

target_include_directories(nip92
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include)

target_link_libraries(nip92 PUBLIC nostr)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include)

# Tests
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_nip92 ${TEST_SOURCES})
    target_include_directories(test_nip92 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/libnostr/include)
    target_link_libraries(test_nip92 nip92 nostr)
    add_test(NAME test_nip92 COMMAND test_nip92)
endif()
