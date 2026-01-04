# Nostr testing and debugging tools

add_executable(test_relay_eose test_relay_eose.c)
target_link_libraries(test_relay_eose PRIVATE nostr libgo)
target_include_directories(test_relay_eose PRIVATE
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
)

# Install to build/tools for easy access
install(TARGETS test_relay_eose DESTINATION ${CMAKE_BINARY_DIR}/tools)
