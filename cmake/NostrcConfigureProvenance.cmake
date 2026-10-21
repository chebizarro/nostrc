# Explicit configure-time metadata; never wraps or replays configure_file.
include_guard(GLOBAL)

function(nostrc_record_configure_output producer template output)
  if(NOT NOSTRC_TEST_SELECTION_TOKEN)
    return()
  endif()
  string(LENGTH "${NOSTRC_TEST_SELECTION_TOKEN}" _length)
  if(NOT ARGC EQUAL 3 OR NOT _length EQUAL 32 OR
      NOT NOSTRC_TEST_SELECTION_TOKEN MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "invalid configure-output provenance request")
  endif()
  foreach(_field producer template output)
    if(NOT IS_ABSOLUTE "${${_field}}" OR NOT EXISTS "${${_field}}" OR
        IS_DIRECTORY "${${_field}}" OR IS_SYMLINK "${${_field}}")
      message(FATAL_ERROR "configure-output provenance requires absolute regular files")
    endif()
    file(SHA256 "${${_field}}" _${_field}_sha256)
    string(HEX "${${_field}}" _${_field}_hex)
  endforeach()
  # Hex paths keep tabs/newlines/list separators out of the record framing.
  file(APPEND "${CMAKE_BINARY_DIR}/nostrc-configure-producers-${NOSTRC_TEST_SELECTION_TOKEN}.tsv"
    "${NOSTRC_TEST_SELECTION_TOKEN}\t${_producer_hex}\t${_template_hex}\t${_output_hex}\t${_producer_sha256}\t${_template_sha256}\t${_output_sha256}\n")
endfunction()
