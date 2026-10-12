# Private profile for every CTest entry of a directory (nostrc-ttvxv,
# nostrc-wqn0u).
#
#   include(${CMAKE_SOURCE_DIR}/cmake/NostrcTestIsolation.cmake)
#   ...every add_test() of the directory...
#   nostrc_isolate_tests(<fixture> [ENVIRONMENT VAR=value...])
#
# Call it last in a CMakeLists.txt: it covers the tests that directory has
# added so far. Each of them then runs with
#   - NOSTRC_TEST_ISOLATION=1, which makes the application code refuse the
#     real profile and key stores (libnostr/include/nostrc-profile-guard.h,
#     gnome/groundhog/src/store/gh-profile-guard.h) and the test helpers abort
#     when the environment still names the real profile
#     (tests/common/nostrc-test-isolation.h);
#   - HOME and XDG_{DATA,CONFIG,CACHE,STATE}_HOME in a private directory of
#     its own, <binary dir>/test-homes/<test>, emptied and recreated on every
#     run by the setup test <fixture>, which every test requires (ctest -R
#     runs it too). These override any HOME / XDG_* a test set itself.
#   - GSETTINGS_BACKEND=memory unless the test chose a backend: the default
#     backends persist outside the private directories (dconf through the
#     session bus, NSUserDefaults on macOS).
# ENVIRONMENT adds variables to every test of the directory, the same way.
# Tests added after the call, such as further setup tests of <fixture>, are
# not covered.
include_guard(GLOBAL)

function(nostrc_isolate_tests fixture)
  cmake_parse_arguments(PARSE_ARGV 1 _nti "" "" "ENVIRONMENT")
  if (_nti_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "nostrc_isolate_tests: unexpected arguments ${_nti_UNPARSED_ARGUMENTS}")
  endif()
  set(_root "${CMAKE_CURRENT_BINARY_DIR}/test-homes")
  set(_script "${CMAKE_CURRENT_BINARY_DIR}/${fixture}.cmake")
  set(_body "file(REMOVE_RECURSE \"${_root}\")\n")
  get_property(_tests DIRECTORY PROPERTY TESTS)
  foreach (_test IN LISTS _tests)
    # Test names may contain '/' (signer/core).
    string(REPLACE "/" "_" _dir "${_test}")
    set(_home "${_root}/${_dir}")
    string(APPEND _body
      "file(MAKE_DIRECTORY \"${_home}/home\" \"${_home}/data\" \"${_home}/config\" "
      "\"${_home}/cache\" \"${_home}/state\")\n")
    get_property(_env TEST ${_test} PROPERTY ENVIRONMENT)
    if (NOT _env MATCHES "(^|;)GSETTINGS_BACKEND=")
      set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT "GSETTINGS_BACKEND=memory")
    endif()
    set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT
      "NOSTRC_TEST_ISOLATION=1" "HOME=${_home}/home" "XDG_DATA_HOME=${_home}/data"
      "XDG_CONFIG_HOME=${_home}/config" "XDG_CACHE_HOME=${_home}/cache"
      "XDG_STATE_HOME=${_home}/state" ${_nti_ENVIRONMENT})
    set_property(TEST ${_test} APPEND PROPERTY FIXTURES_REQUIRED ${fixture})
  endforeach()
  string(APPEND _body "file(CHMOD_RECURSE \"${_root}\" "
    "DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)\n")
  file(WRITE "${_script}" "${_body}")
  add_test(NAME ${fixture} COMMAND ${CMAKE_COMMAND} -P "${_script}")
  set_tests_properties(${fixture} PROPERTIES FIXTURES_SETUP ${fixture})
endfunction()
