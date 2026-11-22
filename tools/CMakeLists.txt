# Nostr testing and debugging tools
set(_vaultctl_default OFF)
if(BUILD_LIBVAULT AND ENABLE_NIP42 AND ENABLE_NIP11)
    set(_vaultctl_default ON)
endif()
option(BUILD_VAULTCTL "Build the shared-vault CLI" ${_vaultctl_default})
unset(_vaultctl_default)
if(BUILD_VAULTCTL)
    add_subdirectory(vaultctl)
endif()

add_executable(test_relay_eose test_relay_eose.c)
target_link_libraries(test_relay_eose PRIVATE nostr libgo)
target_include_directories(test_relay_eose PRIVATE
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
)

# Keep the optional debug tool beneath the installation prefix, never at its
# own build path. Production packaging still omits it with BUILD_TESTING=OFF.
if(BUILD_TESTING)
    install(TARGETS test_relay_eose DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
