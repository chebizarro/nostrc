include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nip70")
  nostrc_add_test_owners("test_nip70" OWNER nip70.test_inputs)
endif()
