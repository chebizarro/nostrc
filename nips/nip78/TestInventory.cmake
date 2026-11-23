include_guard(DIRECTORY)
if(COMMAND nostrc_describe_test AND TEST "test_nip78")
  nostrc_describe_test(test_nip78 OWNER nip78 TARGETS test_nip78
    INPUTS nips/nip78/tests/test_nip78.c nips/nip78/src/nip78.c
      nips/nip78/include/nostr/nip78/nip78.h nips/codec-internal.h)
endif()
if(COMMAND nostrc_describe_test AND TEST "test_nip78_signer")
  nostrc_describe_test(test_nip78_signer OWNER nip78 TARGETS test_nip78_signer
    INPUTS nips/nip78/tests/test_nip78_signer.c nips/nip78/src/nip78.c
      nips/nip78/include/nostr/nip78/nip78.h nips/codec-internal.h)
endif()
