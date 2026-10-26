include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

if(TEST "test_nip65")
  nostrc_add_test_owners("test_nip65" OWNER nip65.test_inputs)
endif()
