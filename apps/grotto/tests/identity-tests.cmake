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
if(TARGET grotto-daemon AND "NIP55L_TEST_TRUST_ENV=1" IN_LIST _nip55l_definitions)
  add_executable(test-profile-integration test-profile-integration.c bunker_test_relay.c ${APP_GRESOURCE_C})
  target_include_directories(test-profile-integration PRIVATE ${SIGNER_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR})
  target_link_libraries(test-profile-integration PRIVATE grotto_app_test_core nostrc-test-bus)
  target_compile_definitions(test-profile-integration PRIVATE PROFILE_DAEMON_BIN="$<TARGET_FILE:grotto-daemon>")
  add_dependencies(test-profile-integration grotto-daemon grotto-schemas)
  apply_test_sanitizers(test-profile-integration)
  add_test(NAME signer/profile-integration COMMAND test-profile-integration)
  set_tests_properties(signer/profile-integration PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "signer;profile;integration"
    ENVIRONMENT "${_UI_TEST_ENV};GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}/..;GSETTINGS_BACKEND=memory;ADW_DISABLE_PORTAL=1")
  if(COMMAND nostrc_describe_test)
    nostrc_describe_test(signer/profile-integration OWNER grotto.gui TARGETS test-profile-integration grotto-daemon)
  endif()
  add_dependencies(signer-tests test-profile-integration)
endif()

# Exercise the same host routes and linked panel library as the application.
foreach(suite onboarding home-model)
  add_executable(test-${suite} test-${suite}.c ${APP_GRESOURCE_C})
  target_link_libraries(test-${suite} PRIVATE grotto_app_test_core nostrc-test-bus)
  target_include_directories(test-${suite} PRIVATE ${SIGNER_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/common)
  apply_test_sanitizers(test-${suite})
  set(_identity_suite ${suite})
  if(suite STREQUAL "home-model")
    set(_identity_suite identity-model)
  endif()
  add_test(NAME signer/${_identity_suite} COMMAND test-${suite})
  set_tests_properties(signer/${_identity_suite} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 LABELS "signer;identity;unit"
      ENVIRONMENT "${_UI_TEST_ENV};GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}/..;GSETTINGS_BACKEND=memory")
  add_dependencies(test-${suite} grotto-schemas grotto_resources)
  add_dependencies(signer-tests test-${suite})
endforeach()
