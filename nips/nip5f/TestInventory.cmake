include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nip5f_loopback")
  nostrc_add_test_owners("test_nip5f_loopback" OWNER nip5f.test_inputs)
endif()
if(TEST "test_nip5f_negative_env")
  nostrc_add_test_owners("test_nip5f_negative_env" OWNER nip5f.test_inputs)
endif()
if(TEST "test_nip5f_tcp")
  nostrc_add_test_owners("test_nip5f_tcp" OWNER nip5f.test_inputs)
  # Unlike loopback/negative-env, this command needs an explicitly provisioned
  # TCP signer endpoint and token. An absent endpoint is not passing coverage.
  set_property(TEST "test_nip5f_tcp" APPEND PROPERTY NOSTRC_SELECTION_REQUIRES signer_endpoint)
  # This directory's deferred collection precedes apps/grotto creation. Retain
  # the required names now; the final File API/descriptor audit resolves them.
  if(TARGET test_nip5f_approver)
    set_property(TEST "test_nip5f_tcp" APPEND PROPERTY NOSTRC_SELECTION_TARGETS
      test_nip5f_tcp test_nip5f_approver grotto-daemon grotto-schemas)
    set_property(TEST "test_nip5f_tcp" APPEND PROPERTY NOSTRC_SELECTION_INPUTS
      "${CMAKE_SOURCE_DIR}/tests/runtime/test_provisioned_nip5f.py"
      "${CMAKE_SOURCE_DIR}/tests/runtime/nip5f_approver.c"
      "${CMAKE_SOURCE_DIR}/gnome/groundhog/tests/app/test_secret_service.py"
      "${CMAKE_SOURCE_DIR}/apps/grotto/data/org.nostr.Grotto.gschema.xml"
      "${CMAKE_BINARY_DIR}/apps/grotto/grotto-daemon"
      "${CMAKE_BINARY_DIR}/nips/nip5f/test_nip5f_tcp"
      "${CMAKE_BINARY_DIR}/nips/nip5f/test_nip5f_approver"
      "${CMAKE_BINARY_DIR}/apps/grotto/gschemas.compiled")
  endif()
endif()
