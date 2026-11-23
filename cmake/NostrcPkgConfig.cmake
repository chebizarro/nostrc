include_guard(GLOBAL)

# Our static SDK uses shared system dependencies. Preserve their ordinary
# pkg-config flags, not their recursive --static implementation closure.
function(nostrc_pkg_config_shared prefix)
  find_package(PkgConfig REQUIRED)
  set(_args ${PKG_CONFIG_ARGN})
  list(REMOVE_ITEM _args --static)
  # Keep target package search paths, but let the eventual consumer apply its
  # own sysroot. Emitted .pc flags must not retain the producer's sysroot.
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PKG_CONFIG_SYSROOT_DIR
    "${PKG_CONFIG_EXECUTABLE}" ${_args} --libs ${ARGN}
    RESULT_VARIABLE _libs_result OUTPUT_VARIABLE _libs
    ERROR_VARIABLE _libs_error OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=PKG_CONFIG_SYSROOT_DIR
    "${PKG_CONFIG_EXECUTABLE}" ${_args} --cflags ${ARGN}
    RESULT_VARIABLE _cflags_result OUTPUT_VARIABLE _cflags
    ERROR_VARIABLE _cflags_error OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _libs_result EQUAL 0 OR NOT _cflags_result EQUAL 0)
    message(FATAL_ERROR "Shared pkg-config dependencies ${ARGN}: ${_libs_error}${_cflags_error}")
  endif()
  set(${prefix}_LIBS "${_libs}" PARENT_SCOPE)
  set(${prefix}_CFLAGS "${_cflags}" PARENT_SCOPE)
endfunction()
