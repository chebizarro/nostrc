project(nip59)

# Core NIP-59 library
add_library(nostr_nip59_core OBJECT
    src/nip59.c
)

target_include_directories(nostr_nip59_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip44/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/libnostr/src
)

target_link_libraries(nostr_nip59_core
    PUBLIC
        nostr
        nostr_nip44_core
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr/nip59/
        DESTINATION include/nostr/nip59
        FILES_MATCHING PATTERN "*.h")

# Tests
if(BUILD_TESTING)
    add_executable(test_nip59 tests/test_nip59.c)
    target_include_directories(test_nip59
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/libnostr/include
            ${CMAKE_SOURCE_DIR}/nips/nip44/include
    )
    target_link_libraries(test_nip59
        nostr_nip59_core
        nostr
        nostr_nip44_core
    )
    add_test(NAME test_nip59 COMMAND test_nip59)
endif()
