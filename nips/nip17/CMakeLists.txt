project(nip17)

# Core NIP-17 library
add_library(nostr_nip17_core OBJECT
    src/nip17.c
)

target_include_directories(nostr_nip17_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip44/include
)

target_link_libraries(nostr_nip17_core
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
    add_executable(test_nip17 tests/test_nip17.c)
    target_include_directories(test_nip17
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/libnostr/include
            ${CMAKE_SOURCE_DIR}/nips/nip44/include
    )
    target_link_libraries(test_nip17
        nostr_nip17_core
        nostr
        nostr_nip44_core
    )
    add_test(NAME test_nip17 COMMAND test_nip17)
endif()

# Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/example.c)
    add_executable(nip17_example examples/example.c)
    target_include_directories(nip17_example
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/libnostr/include
            ${CMAKE_SOURCE_DIR}/nips/nip44/include
    )
    target_link_libraries(nip17_example
        nostr_nip17_core
        nostr
        nostr_nip44_core
    )
endif()
