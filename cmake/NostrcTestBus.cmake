# Defines the static library nostrc-test-bus (tests/common/nostrc-test-bus.c):
# a private D-Bus bus for GLib tests, bound to the test process's lifetime,
# to use instead of GTestDBus. See tests/common/nostrc-test-bus.h.
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcTestBus.cmake)
#   target_link_libraries(my-test PRIVATE nostrc-test-bus)
#
# Safe to include from several components (the first include defines the
# target) and from a component configured on its own, since it only uses
# paths relative to this file. Unix only; needs gio-unix-2.0.
if(NOT TARGET nostrc-test-bus)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NOSTRC_TEST_BUS_GIO REQUIRED IMPORTED_TARGET GLOBAL
    gio-2.0>=2.70 gio-unix-2.0>=2.70)
  get_filename_component(_nostrc_test_bus_dir
    "${CMAKE_CURRENT_LIST_DIR}/../tests/common" ABSOLUTE)
  add_library(nostrc-test-bus STATIC "${_nostrc_test_bus_dir}/nostrc-test-bus.c")
  target_include_directories(nostrc-test-bus PUBLIC "${_nostrc_test_bus_dir}")
  target_link_libraries(nostrc-test-bus PUBLIC PkgConfig::NOSTRC_TEST_BUS_GIO)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nostrc-test-bus PRIVATE -Wall -Wextra -Werror)
  endif()
  # The helper's own test: the macOS EBADF tolerance is armed per test case,
  # bounded and exact (fatal elsewhere). Needs dbus-daemon.
  if(BUILD_TESTING)
    add_executable(nostrc-test-bus-selftest "${_nostrc_test_bus_dir}/nostrc-test-bus-selftest.c")
    target_link_libraries(nostrc-test-bus-selftest PRIVATE nostrc-test-bus)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(nostrc-test-bus-selftest PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME nostrc-test-bus-selftest COMMAND nostrc-test-bus-selftest)
    set_tests_properties(nostrc-test-bus-selftest PROPERTIES TIMEOUT 60
      ENVIRONMENT "G_DEBUG=fatal-criticals")
  endif()
  unset(_nostrc_test_bus_dir)
endif()
