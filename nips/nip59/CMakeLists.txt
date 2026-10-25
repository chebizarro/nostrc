project(nip59 VERSION 0.1.0 LANGUAGES C)

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

# Preserve the OBJECT target for existing in-tree callers; installed consumers
# need a real archive rather than build-tree object paths.
include(GNUInstallDirs)
set_target_properties(nostr_nip59_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(nip59 STATIC $<TARGET_OBJECTS:nostr_nip59_core>)
set_target_properties(nip59 PROPERTIES OUTPUT_NAME nostr_nip59_core)
target_include_directories(nip59 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(nip59 PUBLIC nostr nostr_nip44_core OpenSSL::Crypto)
declare_component_version(nip59 ${PROJECT_VERSION_MAJOR} ${PROJECT_VERSION_MINOR} ${PROJECT_VERSION_PATCH})
set_target_properties(nip59 PROPERTIES COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip59")
apply_versioning(nip59)
configure_file(nip59.pc.in nip59.pc @ONLY)
install(TARGETS nip59 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nip59.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

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
