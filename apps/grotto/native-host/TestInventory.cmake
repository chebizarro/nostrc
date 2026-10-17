include_guard(DIRECTORY)
if(NOT COMMAND nostrc_describe_test)
  return()
endif()

foreach(suite framing policy errors router webln)
  if(TEST "nostr_signer_webext_host_${suite}")
    nostrc_describe_test("nostr_signer_webext_host_${suite}"
      OWNER grotto.native_host TARGETS "test_nmh_${suite}")
  endif()
endforeach()
if(TEST nostr_signer_webext_host_e2e)
  nostrc_describe_test(nostr_signer_webext_host_e2e OWNER grotto.native_host nip55l
    TARGETS test_nmh_e2e nostr-signer-daemon nostr-signer-webext-host REQUIRES private_bus)
endif()
if(TEST nostr_signer_webext_host_webln_e2e)
  nostrc_describe_test(nostr_signer_webext_host_webln_e2e OWNER grotto.native_host gnome.nostr_wallet_agent
    TARGETS test_nmh_webln_e2e nostr-wallet-agent nostr-signer-webext-host REQUIRES private_bus)
endif()
if(TEST nostr_signer_webext_host_webln_paired_e2e)
  nostrc_describe_test(nostr_signer_webext_host_webln_paired_e2e OWNER grotto.native_host gnome.nostr_wallet_agent
    TARGETS test_nmh_webln_paired_e2e nostr-wallet-agent nwa-fixture-wallet nostr-signer-webext-host
    REQUIRES private_bus)
endif()
foreach(suite policy webln)
  if(TEST "nostr_signer_webext_extension_${suite}")
    nostrc_describe_test("nostr_signer_webext_extension_${suite}" OWNER grotto.native_host
      INPUTS "${CMAKE_SOURCE_DIR}/browser-extension/nip07/tests/${suite}.test.js"
        "${CMAKE_SOURCE_DIR}/browser-extension/nip07")
  endif()
endforeach()
