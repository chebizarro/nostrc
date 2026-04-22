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
