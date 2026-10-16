include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nipb0")
  nostrc_add_test_owners("test_nipb0" OWNER nipb0.test_inputs)
endif()
