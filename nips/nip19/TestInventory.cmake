include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test_nip19")
  nostrc_add_test_owners("test_nip19" OWNER nip19.test_inputs)
endif()
if(TEST "test_nip19_tlv")
  nostrc_add_test_owners("test_nip19_tlv" OWNER nip19.test_inputs)
endif()
