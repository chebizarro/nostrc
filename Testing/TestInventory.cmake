include_guard(DIRECTORY)
# Runtime fixtures are read by nostr_testing's NOSTR_TEST_FIXTURES_DIR_DEFAULT.
# The excluded unit case still needs source/header audit ownership; this does
# not claim execution or change its legacy workflow exclusion.
if(COMMAND nostrc_describe_test)
  get_property(_tests DIRECTORY PROPERTY TESTS)
  foreach(_test IN LISTS _tests)
    set(_owners testing.framework)
    if(_test STREQUAL "MockRelayUnit")
      list(APPEND _owners libnostr libjson)
    endif()
    nostrc_describe_test("${_test}" OWNER ${_owners}
      INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  endforeach()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "Nip46MockRelay")
  nostrc_add_test_owners("Nip46MockRelay" OWNER nip46)
endif()
if(TEST "Nip46QRSocket")
  nostrc_add_test_owners("Nip46QRSocket" OWNER nip46)
endif()
if(TEST "SignerIntegration")
  nostrc_add_test_owners("SignerIntegration" OWNER gnome.seahorse nip04 nip06 nip44 nip49 nip55l version.contract)
endif()
