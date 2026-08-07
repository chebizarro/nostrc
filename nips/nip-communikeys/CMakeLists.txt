project(nip_communikeys C)

add_library(nip_communikeys src/nip_communikeys.c)
target_include_directories(nip_communikeys
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/libnostr/include
)
target_link_libraries(nip_communikeys PUBLIC nostr)
set_target_properties(nip_communikeys PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nip_communikeys PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(test_nip_communikeys tests/test_nip_communikeys.c)
    target_link_libraries(test_nip_communikeys PRIVATE nip_communikeys nostr)
    add_test(NAME test_nip_communikeys COMMAND test_nip_communikeys)
endif()
