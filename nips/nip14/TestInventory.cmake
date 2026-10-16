include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nip14")
  nostrc_add_test_owners("test_nip14" OWNER nip14.test_inputs)
endif()
