project(nip51)

# Core library
add_library(nostr_nip51_core OBJECT
    src/nip51.c
)

target_include_directories(nostr_nip51_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip44/include
)

target_link_libraries(nostr_nip51_core
    PUBLIC
        nostr
        nostr_nip44_core
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")

# Tests
if(BUILD_TESTING)
    add_executable(test_nip51 tests/test_nip51.c)
    target_include_directories(test_nip51 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip44/include
    )
    target_link_libraries(test_nip51
        nostr_nip51_core
        nostr
        nostr_nip44_core
    )
    add_test(NAME test_nip51 COMMAND test_nip51)
endif()
