project(nip59)

# Gift-wrap timestamps come from OpenSSL's CSPRNG (RAND_bytes).
find_package(OpenSSL REQUIRED)

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
        OpenSSL::Crypto
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

    # nip59.c rebuilt with a stub CSPRNG, to drive the randomness-failure
    # paths (nostrc-rd8j). Not linked against nostr_nip59_core: it carries
    # its own copy of the source.
    add_executable(test_nip59_rng_failure
        tests/test_nip59_rng_failure.c
        src/nip59.c)
    target_compile_definitions(test_nip59_rng_failure
        PRIVATE NIP59_RAND_BYTES=nip59_test_rand_bytes)
    target_include_directories(test_nip59_rng_failure
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/libnostr/include
            ${CMAKE_SOURCE_DIR}/libnostr/src
            ${CMAKE_SOURCE_DIR}/nips/nip44/include
    )
    target_link_libraries(test_nip59_rng_failure
        nostr
        nostr_nip44_core
        OpenSSL::Crypto
    )
    add_test(NAME test_nip59_rng_failure COMMAND test_nip59_rng_failure)
endif()
