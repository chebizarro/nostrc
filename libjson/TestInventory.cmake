include_guard(DIRECTORY)
if(NOT COMMAND nostrc_describe_test)
  return()
endif()

if(TEST json_glib_off_closure)
  nostrc_describe_test(json_glib_off_closure OWNER libjson
    INPUTS tests/check_glib_off_closure.sh EXPECTED pass)
endif()
