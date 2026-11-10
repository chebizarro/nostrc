include_guard(DIRECTORY)
if(NOT COMMAND nostrc_describe_test)
  return()
endif()
if(TEST "test_nip51")
  nostrc_describe_test(test_nip51 OWNER nip51 TARGETS test_nip51
    INPUTS nips/nip51/tests/test_nip51.c nips/nip51/tests/schema-registry.tsv
      nips/nip51/src/nip51.c nips/nip51/src/nip51-document.c
      nips/nip51/src/nip51-schema.c nips/nip51/include/nostr/nip51/nip51.h
      nips/codec-internal.h)
endif()
if(TEST "test_nip51_failures")
  nostrc_describe_test(test_nip51_failures OWNER nip51 TARGETS test_nip51_failures
    INPUTS nips/nip51/tests/test_nip51_failures.c nips/nip51/tests/faults.h
      nips/nip51/src/nip51.c nips/nip51/src/nip51-document.c
      nips/nip51/src/nip51-schema.c nips/codec-internal.h)
endif()
