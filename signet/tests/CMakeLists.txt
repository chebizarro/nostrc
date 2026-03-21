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

# Register tests with CTest
add_test(NAME signet_smoke COMMAND test_signet_smoke)
add_test(NAME replay_cache COMMAND test_replay_cache)
add_test(NAME policy_engine COMMAND test_policy_engine)
add_test(NAME audit_logger COMMAND test_audit_logger)
add_test(NAME bootstrap_contract COMMAND test_bootstrap_contract)
