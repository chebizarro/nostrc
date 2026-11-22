cmake_minimum_required(VERSION 3.22)
if(NOT TEST_BINARY_DIR)
  message(FATAL_ERROR "Pass a task-owned TEST_BINARY_DIR")
endif()
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
file(WRITE "${TEST_BINARY_DIR}/external-fixture.pc" "
Name: external-fixture
Description: Check shared versus private static dependency flags
Version: 1.0
Libs: -L/opt/nostrc-external-fixture/lib -lexternal-fixture
Libs.private: -lprivate-static-not-installed
Cflags: -I/opt/nostrc-external-fixture/include -DEXTERNAL_PUBLIC
Cflags.private: -DEXTERNAL_STATIC
")
set(ENV{PKG_CONFIG_PATH} "${TEST_BINARY_DIR}")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${TEST_BINARY_DIR}/producer-sysroot")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcPkgConfig.cmake")
nostrc_pkg_config_shared(_shared external-fixture)
if(NOT _shared_LIBS STREQUAL "-L/opt/nostrc-external-fixture/lib -lexternal-fixture" OR
    NOT _shared_CFLAGS STREQUAL "-I/opt/nostrc-external-fixture/include -DEXTERNAL_PUBLIC")
  message(FATAL_ERROR "External shared dependency closure leaked: ${_shared_LIBS}; ${_shared_CFLAGS}")
endif()
execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" --static --libs external-fixture
  RESULT_VARIABLE _result OUTPUT_VARIABLE _static)
if(NOT _result EQUAL 0 OR NOT _static MATCHES "private-static-not-installed" OR
    NOT _static MATCHES "producer-sysroot")
  message(FATAL_ERROR "Fixture did not distinguish recursive static dependencies")
endif()
file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
