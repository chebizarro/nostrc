include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip17)

# Seal and gift-wrap timestamps come from OpenSSL's CSPRNG (RAND_bytes).
find_package(OpenSSL REQUIRED)

# Core NIP-17 library
add_library(nostr_nip17_core OBJECT
    src/nip17.c
)
nostrc_library_visibility(nostr_nip17_core NOSTR_NIP17_API
  nostr/nip17/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip17")

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
        OpenSSL::Crypto
)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
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

# Preserve the OBJECT target for in-tree consumers; packages need an archive.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcNipPackage.cmake")
set_target_properties(nostr_nip17_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(nip17 STATIC $<TARGET_OBJECTS:nostr_nip17_core>)
set_target_properties(nip17 PROPERTIES OUTPUT_NAME nostr_nip17_core)
target_link_libraries(nip17 INTERFACE nostr_nip17_core)
target_compile_definitions(nip17 INTERFACE NOSTR_NIP17_CORE_STATIC_DEFINE)
declare_component_version(nip17 0 1 1)
nostrc_install_nip(nip17 nip17 "nostr >= 2.5.2" "nip44 >= 0.1.1" EXTERNAL_PRIVATE openssl)
