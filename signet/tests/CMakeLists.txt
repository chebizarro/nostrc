# SPDX-License-Identifier: MIT
#
# Signet tests

# Smoke test
add_executable(test_signet_smoke test_signet_smoke.c)
target_link_libraries(test_signet_smoke PRIVATE signet_core)
if (TARGET nostr)
  target_link_libraries(test_signet_smoke PRIVATE nostr)
endif()
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_signet_smoke PRIVATE PkgConfig::GLIB)
endif()
if (TARGET PkgConfig::JSON_GLIB)
  target_link_libraries(test_signet_smoke PRIVATE PkgConfig::JSON_GLIB)
endif()

# Replay cache test
add_executable(test_replay_cache test_replay_cache.c)
target_link_libraries(test_replay_cache PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_replay_cache PRIVATE PkgConfig::GLIB)
endif()

# Policy engine test
add_executable(test_policy_engine test_policy_engine.c)
target_link_libraries(test_policy_engine PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_policy_engine PRIVATE PkgConfig::GLIB)
endif()
if (TARGET PkgConfig::JSON_GLIB)
  target_link_libraries(test_policy_engine PRIVATE PkgConfig::JSON_GLIB)
endif()

# Audit logger test
add_executable(test_audit_logger test_audit_logger.c)
target_link_libraries(test_audit_logger PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_audit_logger PRIVATE PkgConfig::GLIB)
endif()
if (TARGET PkgConfig::JSON_GLIB)
  target_link_libraries(test_audit_logger PRIVATE PkgConfig::JSON_GLIB)
endif()

# Bootstrap/auth contract test
add_executable(test_bootstrap_contract test_bootstrap_contract.c)
target_link_libraries(test_bootstrap_contract PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_bootstrap_contract PRIVATE PkgConfig::GLIB)
endif()
if (TARGET PkgConfig::JSON_GLIB)
  target_link_libraries(test_bootstrap_contract PRIVATE PkgConfig::JSON_GLIB)
endif()

# Store CRUD tests
add_executable(test_store test_store.c)
target_link_libraries(test_store PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_store PRIVATE PkgConfig::GLIB)
endif()

# Key store tests
add_executable(test_key_store test_key_store.c)
target_link_libraries(test_key_store PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_key_store PRIVATE PkgConfig::GLIB)
endif()

# Nostr auth / challenge store tests
add_executable(test_nostr_auth test_nostr_auth.c)
target_link_libraries(test_nostr_auth PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_nostr_auth PRIVATE PkgConfig::GLIB)
endif()

# Capability / policy registry tests
add_executable(test_capability test_capability.c)
target_link_libraries(test_capability PRIVATE signet_core)
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_capability PRIVATE PkgConfig::GLIB)
endif()

# NPA-11: Protocol-level relay pool tests
add_executable(test_relay_pool_protocol test_relay_pool_protocol.c)
target_link_libraries(test_relay_pool_protocol PRIVATE signet_core)
if (TARGET nostr)
  target_link_libraries(test_relay_pool_protocol PRIVATE nostr)
endif()
if (TARGET PkgConfig::GLIB)
  target_link_libraries(test_relay_pool_protocol PRIVATE PkgConfig::GLIB)
endif()

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
  target_link_libraries(test_fido_service PRIVATE OpenSSL::Crypto)
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
  target_link_libraries(test_fido_interop_json PRIVATE OpenSSL::Crypto)
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

# Register tests with CTest
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
if (SIGNET_ENABLE_PASSKEYS)
  add_test(NAME store_passkeys COMMAND test_store_passkeys)
  add_test(NAME fido_service COMMAND test_fido_service)
  add_test(NAME fido_interop_external
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_fido_interop_external_check.sh $<TARGET_FILE:test_fido_interop_json> ${CMAKE_CURRENT_BINARY_DIR}/phase3_interop)
endif()
