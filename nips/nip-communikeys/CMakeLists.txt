include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")

project(nip_communikeys C)

# Communikeys V2 (NIP-CAS-0007) wire format. ABI major 3 establishes the
# explicit public export boundary; it does not change the wire version.
set(NIP_COMMUNIKEYS_VERSION_MAJOR 3)
set(NIP_COMMUNIKEYS_VERSION 3.0.0)

add_library(nip_communikeys src/nip_communikeys.c)
nostrc_library_visibility(nip_communikeys NOSTR_NIP_COMMUNIKEYS_API
  nostr/nip_communikeys/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/nip_communikeys")
target_include_directories(nip_communikeys
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/libnostr/include
        ${CMAKE_SOURCE_DIR}/nips/nip19/include
)
target_link_libraries(nip_communikeys PUBLIC nostr nip19)
set_target_properties(nip_communikeys PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    VERSION ${NIP_COMMUNIKEYS_VERSION}
    SOVERSION ${NIP_COMMUNIKEYS_VERSION_MAJOR}
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nip_communikeys PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(test_nip_communikeys tests/test_nip_communikeys.c)
    target_link_libraries(test_nip_communikeys PRIVATE nip_communikeys nostr nip19)
    add_test(NAME test_nip_communikeys COMMAND test_nip_communikeys)
    set_tests_properties(test_nip_communikeys PROPERTIES LABELS "communikeys;unit")
endif()
