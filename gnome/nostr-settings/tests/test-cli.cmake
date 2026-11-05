# Exercise the real entrypoint without a display or usable session bus.
foreach(_option --version -v --help -h)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY
    "DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/nostr-settings-test-bus"
    "${NSS_EXECUTABLE}" "${_option}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 10)
  if(NOT _rc EQUAL 0 OR NOT _err STREQUAL "")
    message(FATAL_ERROR "${_option}: rc=${_rc}, stderr=${_err}")
  endif()
  if(_option STREQUAL "--version" OR _option STREQUAL "-v")
    if(NOT _out STREQUAL "Nostr Settings ${NSS_VERSION}\n")
      message(FATAL_ERROR "Wrong version output: ${_out}")
    endif()
  elseif(NOT _out MATCHES "--version" OR NOT _out MATCHES "--page" OR NOT _out MATCHES "--help")
    message(FATAL_ERROR "Incomplete help: ${_out}")
  endif()
endforeach()
