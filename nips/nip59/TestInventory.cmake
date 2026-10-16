include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nip59")
  nostrc_add_test_owners("test_nip59" OWNER nip59.test_inputs)
endif()
if(TEST "test_nip59_rng_failure")
  nostrc_add_test_owners("test_nip59_rng_failure" OWNER nip59.test_inputs)
endif()
