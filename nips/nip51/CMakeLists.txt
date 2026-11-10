include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip51)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

# Core library
add_library(nostr_nip51_core OBJECT
    src/nip51.c
    src/nip51-document.c
    src/nip51-schema.c
)
nostrc_library_visibility(nostr_nip51_core NOSTR_NIP51_API
  nostr/nip51/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip51")

target_include_directories(nostr_nip51_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip44/include
)

target_include_directories(nostr_nip51_core PRIVATE ${CMAKE_SOURCE_DIR}/nips)

target_link_libraries(nostr_nip51_core
    PUBLIC
        nostr
        nostr_nip44_core
        PkgConfig::JANSSON
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")

# Tests
if(BUILD_TESTING)
    add_executable(test_nip51 tests/test_nip51.c)
    target_compile_definitions(test_nip51 PRIVATE
      NIP51_SCHEMA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/schema-registry.tsv")
    target_include_directories(test_nip51 PRIVATE
        ${CMAKE_SOURCE_DIR}/nips
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

    # Compile the production sources unchanged except for allocator/crypto symbol
    # substitution, so failures can be deterministic without a shipped test API.
    add_executable(test_nip51_failures tests/test_nip51_failures.c
      src/nip51.c src/nip51-document.c src/nip51-schema.c)
    target_compile_definitions(test_nip51_failures PRIVATE NOSTR_NIP51_CORE_STATIC_DEFINE)
    if(MSVC)
      target_compile_options(test_nip51_failures PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/faults.h")
    else()
      target_compile_options(test_nip51_failures PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/faults.h")
    endif()
    target_include_directories(test_nip51_failures PRIVATE
      ${CMAKE_SOURCE_DIR}/nips
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_BINARY_DIR}/include
      ${CMAKE_SOURCE_DIR}/nips/nip44/include)
    target_link_libraries(test_nip51_failures PRIVATE nostr nostr_nip44_core PkgConfig::JANSSON)
    add_test(NAME test_nip51_failures COMMAND test_nip51_failures)
endif()
