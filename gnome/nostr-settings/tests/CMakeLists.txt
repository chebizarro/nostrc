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

# Production shell, real daemon, private bus and Linux executable-role proof.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET grotto-daemon)
  get_target_property(_w6_defs nostr_nip55l_service COMPILE_DEFINITIONS)
  if("NIP55L_TEST_TRUST_ENV=1" IN_LIST _w6_defs)
    find_program(NSS_XVFB_RUN xvfb-run REQUIRED)
    add_executable(test-nostr-settings-panel-mount test-panel-mount.c)
    target_include_directories(test-nostr-settings-panel-mount PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_options(test-nostr-settings-panel-mount PRIVATE ${NSS_WARNINGS})
    target_compile_definitions(test-nostr-settings-panel-mount PRIVATE
      W6_CONTROL_XML="${CMAKE_SOURCE_DIR}/apps/grotto/data/org.nostr.Grotto.Control1.xml")
    target_link_libraries(test-nostr-settings-panel-mount PRIVATE nostr-settings-ui nostrc-test-bus)
    set(_w6_schemas "${CMAKE_CURRENT_BINARY_DIR}/mount-schemas")
    add_custom_command(OUTPUT "${_w6_schemas}/gschemas.compiled"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_w6_schemas}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/apps/grotto/data/org.nostr.Grotto.gschema.xml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Settings.gschema.xml" "${_w6_schemas}"
      COMMAND ${NSS_GLIB_COMPILE_SCHEMAS} --strict "${_w6_schemas}"
      DEPENDS ../data/org.nostr.Settings.gschema.xml
        "${CMAKE_SOURCE_DIR}/apps/grotto/data/org.nostr.Grotto.gschema.xml")
    add_custom_target(nostr-settings-mount-schemas DEPENDS "${_w6_schemas}/gschemas.compiled")
    add_dependencies(test-nostr-settings-panel-mount grotto-daemon grotto-schemas nostr-settings-schema nostr-settings-mount-schemas)
    add_test(NAME nostr-settings/panel-mount COMMAND ${NSS_XVFB_RUN} -a
      $<TARGET_FILE:test-nostr-settings-panel-mount> $<TARGET_FILE:grotto-daemon>)
    set_tests_properties(nostr-settings/panel-mount PROPERTIES LABELS "nostr-settings;signer;integration;security"
      TIMEOUT 120 ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${_w6_schemas};GSETTINGS_BACKEND=memory;GTK_A11Y=test")
  endif()
endif()

# Desktop entry lint.
if (NSS_DESKTOP_FILE_VALIDATE)
  add_test(NAME nostr-settings-desktop-file
           COMMAND ${NSS_DESKTOP_FILE_VALIDATE} ${CMAKE_BINARY_DIR}/gnome/nostr-settings/org.nostr.Settings.desktop)
  set_tests_properties(nostr-settings-desktop-file PROPERTIES LABELS "nostr-settings")
endif()
