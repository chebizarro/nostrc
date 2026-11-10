include_guard(DIRECTORY)
if(COMMAND nostrc_describe_test AND TEST "test_nip37")
  nostrc_describe_test(test_nip37 OWNER nip37 TARGETS test_nip37
    INPUTS nips/nip37/tests/test_nip37.c nips/nip37/src/nip37.c
      nips/nip37/include/nostr/nip37/nip37.h nips/codec-internal.h)
endif()
