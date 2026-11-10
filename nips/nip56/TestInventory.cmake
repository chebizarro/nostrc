include_guard(DIRECTORY)
if(COMMAND nostrc_describe_test AND TEST "test_nip56")
  nostrc_describe_test(test_nip56 OWNER nip56 TARGETS test_nip56
    INPUTS nips/nip56/tests/test_nip56.c nips/nip56/src/nip56.c
      nips/nip56/include/nostr/nip56/nip56.h nips/codec-internal.h)
endif()
