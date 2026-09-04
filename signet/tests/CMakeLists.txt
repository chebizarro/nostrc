# SPDX-License-Identifier: MIT
#
# Signet tests
#
# PARITY RULE (fp-52iq): this file and tests/meson.build must build and run the
# same test set. Six tests plus test_signetctl_credentials.sh once existed only
# under meson, so the CMake CI that ships the project silently covered less than
# the developer build -- the same "green but never executed" gap as fp-3126.
# Add a test to BOTH lists, or record the reason for the omission next to it.

# fp-3126: the repo builds Release, which defines NDEBUG, under which
# assert(expr) expands to ((void)0) and never evaluates its argument. The
# signet suite used bare assert() for side-effecting setup calls, so ctest
# passed having executed nothing. Tests now use CHECK() from test_check.h,
# but keep asserts live in test translation units as a second line of
# defence -- a test file that forgets the header still gets working checks.
# Directory-scoped, so it also covers ../src sources compiled into tests.
if (NOT MSVC)
  add_compile_options(-UNDEBUG)
endif()

# One helper so every test gets the same link and include set that meson's
# `signet_deps + signet_includes` provides. Previously each test hand-listed a
# subset of libraries, which is what let the two build systems drift: adding a
# test to CMake meant reproducing a 15-line block, so it was easy to skip.
function(signet_add_test_executable name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE signet_core)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
    ${CMAKE_SOURCE_DIR}/nips/nip04/include
    ${CMAKE_SOURCE_DIR}/nips/nip17/include
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/nips/nip44/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
  )
  foreach (dep nostr nip19 nip44 nip04 nip11 nostr_nip17_core nostr_nip46_core)
    if (TARGET ${dep})
      target_link_libraries(${name} PRIVATE ${dep})
    endif()
  endforeach()
  foreach (pcdep GLIB JSON_GLIB SQLITE3 SODIUM)
    if (TARGET PkgConfig::${pcdep})
      target_link_libraries(${name} PRIVATE PkgConfig::${pcdep})
    endif()
  endforeach()
endfunction()

# Test executables. Keep in step with the executable() list in meson.build.
signet_add_test_executable(test_signet_smoke)             # smoke
signet_add_test_executable(test_replay_cache)
signet_add_test_executable(test_policy_engine)
signet_add_test_executable(test_audit_logger)
signet_add_test_executable(test_bootstrap_contract)       # bootstrap/auth contract
signet_add_test_executable(test_store)                    # store CRUD
signet_add_test_executable(test_key_store)
signet_add_test_executable(test_nostr_auth)               # challenge store
signet_add_test_executable(test_capability)               # policy registry
signet_add_test_executable(test_relay_pool_protocol)      # NPA-11
signet_add_test_executable(test_config_reload)            # SIGHUP reload (fp-56t)
signet_add_test_executable(test_signing_crypto)           # NIP-44 round-trip
signet_add_test_executable(test_store_lifecycle)          # audit chain, revoke precedence
signet_add_test_executable(test_db_migration)             # plaintext -> SQLCipher
signet_add_test_executable(test_adopt_existing)
signet_add_test_executable(test_reissue_connect)
signet_add_test_executable(test_mgmt_replay)
signet_add_test_executable(test_pubkey_backfill)          # legacy agents.pubkey backfill
signet_add_test_executable(test_reissue_self_service)
signet_add_test_executable(test_client_bindings)          # persistent NIP-46 bindings
signet_add_test_executable(test_credential_lifecycle)
signet_add_test_executable(test_credential_access)
signet_add_test_executable(test_git_credential)
signet_add_test_executable(test_backup_restore)
signet_add_test_executable(test_item3_concurrency)        # connection/port robustness

if (SIGNET_ENABLE_PASSKEYS)
  add_executable(test_store_passkeys
    test_store_passkeys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/store_passkeys.c
  )
  target_include_directories(test_store_passkeys PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  target_link_libraries(test_store_passkeys PRIVATE OpenSSL::Crypto)
  if (TARGET PkgConfig::SQLITE3)
    target_link_libraries(test_store_passkeys PRIVATE PkgConfig::SQLITE3)
  else()
    target_link_libraries(test_store_passkeys PRIVATE sqlite3)
  endif()
  if (TARGET PkgConfig::SODIUM)
    target_link_libraries(test_store_passkeys PRIVATE PkgConfig::SODIUM)
  else()
    target_link_libraries(test_store_passkeys PRIVATE sodium)
  endif()

  add_executable(test_fido_service
    test_fido_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fido.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fido_crypto_openssl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fido_cbor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/store_passkeys.c
  )
  target_include_directories(test_fido_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  target_compile_definitions(test_fido_service PRIVATE SIGNET_ENABLE_PASSKEYS=1)
  target_link_libraries(test_fido_service PRIVATE OpenSSL::Crypto PkgConfig::LIBCBOR)
  if (TARGET PkgConfig::GLIB)
    target_link_libraries(test_fido_service PRIVATE PkgConfig::GLIB)
  endif()
  if (TARGET PkgConfig::JSON_GLIB)
    target_link_libraries(test_fido_service PRIVATE PkgConfig::JSON_GLIB)
  endif()
  if (TARGET PkgConfig::SQLITE3)
    target_link_libraries(test_fido_service PRIVATE PkgConfig::SQLITE3)
  else()
    target_link_libraries(test_fido_service PRIVATE sqlite3)
  endif()
  if (TARGET PkgConfig::SODIUM)
    target_link_libraries(test_fido_service PRIVATE PkgConfig::SODIUM)
  else()
    target_link_libraries(test_fido_service PRIVATE sodium)
  endif()

  add_executable(test_fido_ctaphid test_fido_ctaphid.c)
  target_link_libraries(test_fido_ctaphid PRIVATE signet_core PkgConfig::LIBCBOR)

  add_executable(test_fido_interop_json
    test_fido_interop_json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fido.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fido_crypto_openssl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/fido_cbor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/store_passkeys.c
  )
  target_include_directories(test_fido_interop_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  target_compile_definitions(test_fido_interop_json PRIVATE SIGNET_ENABLE_PASSKEYS=1)
  target_link_libraries(test_fido_interop_json PRIVATE OpenSSL::Crypto PkgConfig::LIBCBOR)
  if (TARGET PkgConfig::GLIB)
    target_link_libraries(test_fido_interop_json PRIVATE PkgConfig::GLIB)
  endif()
  if (TARGET PkgConfig::JSON_GLIB)
    target_link_libraries(test_fido_interop_json PRIVATE PkgConfig::JSON_GLIB)
  endif()
  if (TARGET PkgConfig::SQLITE3)
    target_link_libraries(test_fido_interop_json PRIVATE PkgConfig::SQLITE3)
  else()
    target_link_libraries(test_fido_interop_json PRIVATE sqlite3)
  endif()
  if (TARGET PkgConfig::SODIUM)
    target_link_libraries(test_fido_interop_json PRIVATE PkgConfig::SODIUM)
  else()
    target_link_libraries(test_fido_interop_json PRIVATE sodium)
  endif()
endif()

# Register tests with CTest. Keep in step with the test() list in meson.build.

# fp-3126 guard: fail the suite if a bare assert() reappears anywhere in the
# signet test tree. Catches new test files that never include test_check.h.
add_test(NAME no_bare_assert
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_no_bare_assert.sh ${CMAKE_CURRENT_SOURCE_DIR})

# fp-52iq guard: fail the suite if the CMake and meson test lists drift apart
# again. Enforces the parity rule at the top of this file.
add_test(NAME test_list_parity
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_test_parity.sh ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME signet_smoke COMMAND test_signet_smoke)
add_test(NAME replay_cache COMMAND test_replay_cache)
add_test(NAME policy_engine COMMAND test_policy_engine)
add_test(NAME audit_logger COMMAND test_audit_logger)
add_test(NAME bootstrap_contract COMMAND test_bootstrap_contract)
add_test(NAME store COMMAND test_store)
add_test(NAME key_store COMMAND test_key_store)
add_test(NAME nostr_auth COMMAND test_nostr_auth)
add_test(NAME capability COMMAND test_capability)
add_test(NAME relay_pool_protocol COMMAND test_relay_pool_protocol)
add_test(NAME config_reload COMMAND test_config_reload)
add_test(NAME signing_crypto COMMAND test_signing_crypto)
add_test(NAME store_lifecycle COMMAND test_store_lifecycle)
add_test(NAME db_migration COMMAND test_db_migration)
add_test(NAME adopt_existing COMMAND test_adopt_existing)
add_test(NAME reissue_connect COMMAND test_reissue_connect)
add_test(NAME mgmt_replay COMMAND test_mgmt_replay)
add_test(NAME pubkey_backfill COMMAND test_pubkey_backfill)
add_test(NAME reissue_self_service COMMAND test_reissue_self_service)
add_test(NAME client_bindings COMMAND test_client_bindings)
add_test(NAME credential_lifecycle COMMAND test_credential_lifecycle)
add_test(NAME credential_access COMMAND test_credential_access)
add_test(NAME git_credential COMMAND test_git_credential)
add_test(NAME signetctl_credentials
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_signetctl_credentials.sh $<TARGET_FILE:signetctl>)
add_test(NAME backup_restore COMMAND test_backup_restore)
add_test(NAME item3_concurrency COMMAND test_item3_concurrency)

# Match the meson timeouts for the three tests that do real socket/threading
# work; CTest's default is 1500s, which would let a hang stall CI.
set_tests_properties(config_reload item3_concurrency backup_restore
  PROPERTIES TIMEOUT 60)

if (SIGNET_ENABLE_PASSKEYS)
  add_test(NAME store_passkeys COMMAND test_store_passkeys)
  add_test(NAME fido_service COMMAND test_fido_service)
  add_test(NAME fido_ctaphid COMMAND test_fido_ctaphid)
  add_test(NAME fido_interop_external
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_fido_interop_external_check.sh $<TARGET_FILE:test_fido_interop_json> ${CMAKE_CURRENT_BINARY_DIR}/phase3_interop)

  # CMake-only, deliberately: needs a live session D-Bus and real hardware
  # interaction, so it is registered DISABLED for manual invocation and has no
  # meson counterpart. Not part of the parity rule above.
  add_test(NAME live_dbus_interop_manual
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/interop/run_live_dbus_interop.sh)
  set_tests_properties(live_dbus_interop_manual PROPERTIES
    DISABLED TRUE
    LABELS "manual;interop;dbus;passkeys")
endif()
