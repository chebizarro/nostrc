# SPDX-License-Identifier: MIT
#
# Signet tests

test_exe = executable('test_signet_smoke',
  'test_signet_smoke.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: false,
)

test_replay = executable('test_replay_cache',
  'test_replay_cache.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: false,
)

test_policy = executable('test_policy_engine',
  'test_policy_engine.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: false,
)

test_audit = executable('test_audit_logger',
  'test_audit_logger.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: false,
)

test('signet_smoke', test_exe)
test('replay_cache', test_replay)
test('policy_engine', test_policy)
test('audit_logger', test_audit)