include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test-nip34")
  nostrc_add_test_owners("test-nip34" OWNER nip34.test_inputs)
endif()
