include_guard(DIRECTORY)
if(NOT COMMAND nostrc_describe_test)
  return()
endif()

if(TEST homed_acceptance_runner_contract)
  nostrc_describe_test(homed_acceptance_runner_contract
    OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs
    INPUTS tests/acceptance/test_acceptance_runner.py tests/acceptance/run_acceptance.py
           tests/acceptance/matrix.json EXPECTED pass)
endif()

if(TEST homed_porthome_at_rest)
  nostrc_describe_test(homed_porthome_at_rest OWNER gnome.nostr_homed
    TARGETS test_porthome_at_rest EXPECTED pass)
endif()

# The opt-in auth/identity/roaming runtimes are not enabled by these units.
if(TEST homed_manifest)
  nostrc_describe_test(homed_manifest OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs TARGETS test_manifest EXPECTED pass)
endif()
if(TEST homed_cache)
  nostrc_describe_test(homed_cache OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs TARGETS test_cache EXPECTED pass)
endif()
if(TEST homed_cache_config)
  nostrc_describe_test(homed_cache_config OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs TARGETS test_cache_config EXPECTED pass)
endif()
if(TEST homed_uid_map)
  nostrc_describe_test(homed_uid_map OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs TARGETS test_uid_map EXPECTED pass)
endif()
if(TEST homed_groups)
  nostrc_describe_test(homed_groups OWNER gnome.nostr_homed gnome.nostr_homed.test_inputs TARGETS test_groups EXPECTED pass)
endif()

if(TEST homed_syncd_vault_protocol)
  nostrc_describe_test(homed_syncd_vault_protocol OWNER gnome.nostr_homed
    INPUTS tests/integration/test_syncd_vault_protocol.py
           ${CMAKE_SOURCE_DIR}/tools/vaultctl/tests/fixtures.py EXPECTED pass)
endif()
if(TEST homed_syncd_vault_config)
  nostrc_describe_test(homed_syncd_vault_config OWNER gnome.nostr_homed
    INPUTS tests/integration/test_syncd_vault_config.py EXPECTED pass)
endif()
foreach(_closure_test homed_syncd_off_closure homed_vault_pam_nostr_closure homed_vault_nss_nostr_closure)
  if(TEST ${_closure_test})
    nostrc_describe_test(${_closure_test} OWNER gnome.nostr_homed
      INPUTS tests/integration/test_syncd_off_closure.sh EXPECTED pass)
  endif()
endforeach()
