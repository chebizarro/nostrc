# nostr-settings tests: no display, no network, no real signer/systemd.
# D-Bus peers (systemd Manager, SessionRelay1, Wallet1) are mocks on a
# private GTestDBus bus; relays are libnostr-publish fixture transports.

set(_nss_tests config lists nip11 relay_stats systemd wallet identity)
foreach(_t ${_nss_tests})
  add_executable(test-nostr-settings-${_t} test_${_t}.c)
  target_compile_options(test-nostr-settings-${_t} PRIVATE ${NSS_WARNINGS})
  target_link_libraries(test-nostr-settings-${_t} PRIVATE nostr-settings-core)
  add_test(NAME nostr-settings-${_t} COMMAND test-nostr-settings-${_t})
  set_tests_properties(nostr-settings-${_t} PROPERTIES
    LABELS "nostr-settings;unit" TIMEOUT 120
    ENVIRONMENT "GSETTINGS_BACKEND=memory;NO_AT_BRIDGE=1")
endforeach()

target_compile_definitions(test-nostr-settings-relay_stats PRIVATE
  NSS_RELAY_XML="${CMAKE_SOURCE_DIR}/gnome/dbus/org.nostr.SessionRelay1.xml")

# Cross-check: files written by the Settings stores parse with the owning
# tools' own parsers (when those components are part of this build).
if (TARGET nostr_seal_core)
  target_link_libraries(test-nostr-settings-config PRIVATE nostr_seal_core)
  target_compile_definitions(test-nostr-settings-config PRIVATE NSS_TEST_SEAL_PARSER=1)
endif()
if (TARGET nostr-share-core)
  target_link_libraries(test-nostr-settings-config PRIVATE nostr-share-core)
  target_compile_definitions(test-nostr-settings-config PRIVATE NSS_TEST_SHARE_PARSER=1)
endif()
if (TARGET nostr-relay-server)
  target_link_libraries(test-nostr-settings-config PRIVATE nostr-relay-server)
  target_compile_definitions(test-nostr-settings-config PRIVATE NSS_TEST_RELAYD_PARSER=1)
endif()

if (TARGET nostr-wallet-agent-core)
  target_link_libraries(test-nostr-settings-wallet PRIVATE nostr-wallet-agent-core)
  target_compile_definitions(test-nostr-settings-wallet PRIVATE NSS_TEST_WALLET_WRITER=1)
endif()

# Desktop entry lint.
if (NSS_DESKTOP_FILE_VALIDATE)
  add_test(NAME nostr-settings-desktop-file
           COMMAND ${NSS_DESKTOP_FILE_VALIDATE} ${CMAKE_BINARY_DIR}/gnome/nostr-settings/org.nostr.Settings.desktop)
  set_tests_properties(nostr-settings-desktop-file PROPERTIES LABELS "nostr-settings")
endif()
