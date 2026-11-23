cmake_minimum_required(VERSION 3.22)
if(NOT TEST_BINARY_DIR)
  message(FATAL_ERROR "Pass a task-owned TEST_BINARY_DIR")
endif()
if(NOT TEST_JOBS)
  set(TEST_JOBS 2)
endif()
set(_configure "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/external-pc"
  -B "${TEST_BINARY_DIR}")
if(PKG_CONFIG_EXECUTABLE)
  list(APPEND _configure "-DPKG_CONFIG_EXECUTABLE=${PKG_CONFIG_EXECUTABLE}")
endif()
execute_process(COMMAND ${_configure} COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}" -j "${TEST_JOBS}"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${TEST_BINARY_DIR}"
  --output-on-failure COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
