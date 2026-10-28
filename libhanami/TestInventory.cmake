include_guard(DIRECTORY)
if(NOT COMMAND nostrc_add_test_owners)
  return()
endif()

if(TEST "test_hanami_sealed_roundtrip")
  nostrc_describe_test("test_hanami_sealed_roundtrip"
    OWNER libhanami nip34
    TARGETS test_hanami_sealed_roundtrip
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/sealed_fixture.py"
    EXPECTED pass)
endif()

if(TEST "test-hanami-clone")
  nostrc_describe_test("test-hanami-clone"
    OWNER libhanami nip34
    TARGETS test-hanami-clone
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/clone_fixture.py"
    EXPECTED pass)
endif()

# Exact registration/input relationships retained from the ab5 production audit.
if(TEST "test-hanami-config")
  nostrc_add_test_owners("test-hanami-config" OWNER nip34)
endif()
if(TEST "test-hanami-grasp")
  nostrc_add_test_owners("test-hanami-grasp" OWNER nip34)
endif()
if(TEST "test-hanami-index")
  nostrc_add_test_owners("test-hanami-index" OWNER nip34)
endif()
if(TEST "test-hanami-integration")
  nostrc_add_test_owners("test-hanami-integration" OWNER nip34)
endif()
if(TEST "test-hanami-nostr-protocol")
  nostrc_add_test_owners("test-hanami-nostr-protocol" OWNER nip34)
endif()
if(TEST "test-hanami-nostr")
  nostrc_add_test_owners("test-hanami-nostr" OWNER nip34)
endif()
if(TEST "test-hanami-odb-backend")
  nostrc_add_test_owners("test-hanami-odb-backend" OWNER nip34)
endif()
if(TEST "test-hanami-refdb-backend")
  nostrc_add_test_owners("test-hanami-refdb-backend" OWNER nip34)
endif()
if(TEST "test-hanami-transport")
  nostrc_add_test_owners("test-hanami-transport" OWNER nip34)
endif()
if(TEST "test-hanami-types")
  nostrc_add_test_owners("test-hanami-types" OWNER nip34)
endif()
