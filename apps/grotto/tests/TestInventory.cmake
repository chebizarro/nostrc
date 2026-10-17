include_guard(DIRECTORY)
if(NOT COMMAND nostrc_describe_test)
  return()
endif()

# Registration names are independent executions, including same-binary aliases.
function(_grotto_test name target owner)
  if(TEST "${name}")
    cmake_parse_arguments(PARSE_ARGV 3 ARG "" "" "INPUTS;TARGETS;REQUIRES")
    get_property(_env TEST "${name}" PROPERTY ENVIRONMENT)
    if(_env MATCHES "GSETTINGS_SCHEMA_DIR=" AND TARGET grotto-schemas)
      list(APPEND ARG_TARGETS grotto-schemas)
      list(APPEND ARG_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Grotto.gschema.xml")
    endif()
    nostrc_describe_test("${name}" OWNER ${owner} TARGETS ${target} ${ARG_TARGETS}
      INPUTS ${ARG_INPUTS} REQUIRES ${ARG_REQUIRES})
  endif()
endfunction()

foreach(suite crypto identity session legacy-dirs rate-limiter hsm-provider core
    secure-mem social-recovery signer-core profile-guard)
  _grotto_test("signer/${suite}" "test-${suite}" grotto.gui)
endforeach()
_grotto_test(signer/dbus test-dbus grotto.gui REQUIRES private_bus)
_grotto_test(signer/ui test-ui grotto.gui REQUIRES display)
_grotto_test(signer/qr-scanner test-qr-scanner grotto.gui REQUIRES display)
_grotto_test(signer/approval-dialog test-approval-dialog grotto.gui REQUIRES display)
_grotto_test(signer/bunker-startup test-bunker-startup grotto.daemon)
_grotto_test(signer/bunker test-bunker-startup grotto.daemon)
foreach(suite bunker-e2e bunker-headless)
  _grotto_test("signer/${suite}" test-bunker-e2e grotto.daemon
    TARGETS grotto-daemon grotto-schemas REQUIRES private_bus
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Grotto.gschema.xml")
endforeach()
foreach(suite clients activity)
  _grotto_test("signer/${suite}" "test-${suite}" grotto.daemon
    TARGETS grotto-daemon test-control-untrusted REQUIRES private_bus)
endforeach()
_grotto_test(signer/approval-controller test-approval-controller grotto.gui REQUIRES display private_bus)
_grotto_test(signer/approval-integration test-approval-integration "grotto.gui;grotto.daemon"
  TARGETS grotto-daemon REQUIRES display private_bus INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
_grotto_test(signer/room-flow test-room-flow "grotto.gui;grotto.daemon"
  TARGETS grotto-daemon room-flow-schemas REQUIRES display private_bus
  INPUTS "${CMAKE_SOURCE_DIR}/gnome/groundhog/data/org.nostr.Groundhog.gschema.xml")
_grotto_test(signer/identity-management test-identity-management grotto.daemon
  TARGETS grotto-daemon test-identity-consumer-a test-identity-consumer-b REQUIRES private_bus)
_grotto_test(signer/onboarding test-onboarding grotto.gui REQUIRES display private_bus
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
_grotto_test(signer/identity-model test-home-model grotto.gui REQUIRES display private_bus)
_grotto_test(signer/home-model test-home-dashboard grotto.gui REQUIRES private_bus)
foreach(suite ui-layout ui-accessibility)
  _grotto_test("signer/${suite}" test-ui-layout grotto.gui
    TARGETS grotto_test_locales grotto_resources REQUIRES display private_bus
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Grotto.Control1.xml"
      "${CMAKE_CURRENT_SOURCE_DIR}/../po/es.po")
endforeach()
_grotto_test(signer/background test-background grotto.gui REQUIRES private_bus
  INPUTS "${CMAKE_CURRENT_BINARY_DIR}/../org.nostr.Grotto-autostart.desktop"
    "${CMAKE_CURRENT_SOURCE_DIR}/../data/org.nostr.Grotto-autostart.desktop.in")
_grotto_test(signer/session-lock test-session-lock grotto.daemon REQUIRES private_bus)

if(TEST signer/blueprint-drift)
  nostrc_describe_test(signer/blueprint-drift OWNER grotto.gui
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/test-blueprint-drift.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/../data/ui")
endif()
if(TEST signer/desktop-contract)
  set(_worker)
  if(TARGET test-desktop-activation)
    list(APPEND _worker test-desktop-activation)
  endif()
  nostrc_describe_test(signer/desktop-contract OWNER grotto.gui grotto.daemon
    TARGETS grotto grotto-schemas ${_worker} REQUIRES display private_bus
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/test-desktop-contract.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt"
      "${CMAKE_CURRENT_SOURCE_DIR}/../meson.build"
      "${CMAKE_CURRENT_SOURCE_DIR}/../data"
      "${CMAKE_CURRENT_SOURCE_DIR}/../src"
      "${CMAKE_CURRENT_SOURCE_DIR}/../packaging"
      "${CMAKE_CURRENT_SOURCE_DIR}/../daemon/main_daemon.c"
      "${CMAKE_CURRENT_SOURCE_DIR}/../daemon/packaging/systemd/user"
      "${CMAKE_SOURCE_DIR}/flake.nix"
      "${CMAKE_CURRENT_BINARY_DIR}/../org.nostr.Grotto-autostart.desktop"
      "${CMAKE_CURRENT_BINARY_DIR}/../org.nostr.Grotto.service"
      "${CMAKE_CURRENT_BINARY_DIR}/../org.nostr.Signer.service")
endif()
