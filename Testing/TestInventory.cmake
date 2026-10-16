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
