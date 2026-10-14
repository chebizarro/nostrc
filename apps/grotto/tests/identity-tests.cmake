# G09: real custody/gate and real GUI state, mocking only external bus I/O.
if (TARGET grotto_control_core AND "NIP55L_TEST_TRUST_ENV=1" IN_LIST _nip55l_definitions)
  foreach(consumer a b)
    add_executable(test-identity-consumer-${consumer} test-identity-consumer.c)
    target_include_directories(test-identity-consumer-${consumer} PRIVATE ${SIGNER_INCLUDE_DIRS})
    target_link_libraries(test-identity-consumer-${consumer} PRIVATE ${SIGNER_TEST_LIBS})
  endforeach()
  add_executable(test-identity-management test-identity-management.c)
  target_include_directories(test-identity-management PRIVATE ${CMAKE_SOURCE_DIR} ${SIGNER_INCLUDE_DIRS}
      ${CMAKE_SOURCE_DIR}/nips/nip55l/src/glib)
  target_link_libraries(test-identity-management PRIVATE grotto_control_core nostrc-test-bus)
  if(LIBSECRET_FOUND)
    target_compile_definitions(test-identity-management PRIVATE G09_HAVE_LIBSECRET=1)
    target_link_libraries(test-identity-management PRIVATE PkgConfig::LIBSECRET)
  endif()
  target_compile_definitions(test-identity-management PRIVATE NIP55L_TEST_TRUST_ENV=1
      IDENTITY_DAEMON_BIN="$<TARGET_FILE:grotto-daemon>"
      CONSUMER_A_BIN="$<TARGET_FILE:test-identity-consumer-a>"
      CONSUMER_B_BIN="$<TARGET_FILE:test-identity-consumer-b>")
  add_dependencies(test-identity-management grotto-daemon grotto-schemas test-identity-consumer-a test-identity-consumer-b)
  add_test(NAME signer/identity-management COMMAND test-identity-management)
  set_tests_properties(signer/identity-management PROPERTIES TIMEOUT 120 LABELS "signer;control;integration"
      ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}/..;GSETTINGS_BACKEND=memory")
  add_dependencies(signer-tests test-identity-management)
endif()
add_library(grotto_identity_ui_test_core STATIC
    ../src/accounts_store.c ../src/settings_manager.c ../src/secret_store.c
    ../src/secure-mem.c ../src/secure-memory.c ../src/secure-delete.c
    ../src/key_provider.c ../src/key_provider_secp256k1.c ../src/key_provider_ed25519.c
    ../src/gn-signer-error.c ../src/backup-recovery.c
    ../src/secret-clipboard.c ../src/ui/onboarding-assistant.c
    ../src/ui/widgets/gn-secure-entry.c ../src/ui/page-identity-detail.c
    ../src/ui/sheets/sheet-qr-display.c ../src/qr-code.c)
apply_test_sanitizers(grotto_identity_ui_test_core)
target_include_directories(grotto_identity_ui_test_core PUBLIC ${SIGNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests/common)
target_link_libraries(grotto_identity_ui_test_core PUBLIC ${SIGNER_TEST_LIBS} nostr_gobject
    PkgConfig::GTK4 PkgConfig::ADWAITA)
if(APPLE)
  target_link_libraries(grotto_identity_ui_test_core PRIVATE ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
endif()
if(QRENCODE_FOUND)
  target_link_libraries(grotto_identity_ui_test_core PRIVATE PkgConfig::QRENCODE)
  target_compile_definitions(grotto_identity_ui_test_core PRIVATE HAVE_QRENCODE=1)
endif()
foreach(suite onboarding home-model)
  add_executable(test-${suite} test-${suite}.c ${APP_GRESOURCE_C})
  target_link_libraries(test-${suite} PRIVATE grotto_identity_ui_test_core nostrc-test-bus)
  apply_test_sanitizers(test-${suite})
  add_test(NAME signer/${suite} COMMAND test-${suite})
  set_tests_properties(signer/${suite} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 LABELS "signer;identity;unit"
      ENVIRONMENT "${_UI_TEST_ENV};GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}/..;GSETTINGS_BACKEND=memory")
  add_dependencies(test-${suite} grotto-schemas)
  add_dependencies(signer-tests test-${suite})
endforeach()
