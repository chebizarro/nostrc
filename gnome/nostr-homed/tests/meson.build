# Portable test inventory mirrors the CMake names exactly.
if get_option('auth_core').enabled()
  auth_protocol_test = executable('test_auth_protocol', 'unit/test_auth_protocol.c',
    include_directories: inc, dependencies: auth_core_dep)
  test('homed_auth_protocol', auth_protocol_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'auth'])

  auth_transaction_test = executable('test_auth_transaction', 'unit/test_auth_transaction.c',
    include_directories: inc, dependencies: auth_core_dep)
  test('homed_auth_transaction', auth_transaction_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'auth'])

  auth_vault_test = executable('test_auth_vault', 'unit/test_auth_vault.c',
    include_directories: inc, dependencies: auth_core_dep)
  test('homed_auth_vault', auth_vault_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'auth'])

  auth_challenge_test = executable('test_auth_challenge', 'unit/test_auth_challenge.c',
    include_directories: inc, dependencies: auth_core_dep)
  test('homed_auth_challenge', auth_challenge_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'auth'])

  provider_local_test = executable('test_provider_local', 'integration/test_provider_local.c',
    include_directories: inc, dependencies: auth_core_dep)
  test('homed_provider_local', provider_local_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'auth'])
endif

if get_option('identity_core').enabled()
  identity_store_test = executable('test_identity_store', 'identity/test_identity_store.c',
    include_directories: inc, dependencies: identity_core_dep)
  test('homed_identity_store', identity_store_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'identity'])

  identity_config_test = executable('test_identity_config', 'identity/test_identity_config.c',
    include_directories: inc, dependencies: identity_core_dep)
  test('homed_identity_config', identity_config_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'identity'])

  identity_fixtures_test = executable('test_identity_fixtures', 'identity/test_identity_fixtures.c',
    include_directories: inc,
    dependencies: identity_core_dep,
    c_args: ['-DIDENTITY_FIXTURE_DIR="' + meson.current_source_dir() / 'identity/fixtures' + '"'])
  test('homed_identity_fixtures', identity_fixtures_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'identity'])
endif

if get_option('auth_core').enabled() and get_option('identity_core').enabled()
  auth_identity_test = executable('test_auth_transaction_identity',
    'integration/test_auth_transaction_identity.c',
    include_directories: inc, dependencies: auth_identity_dep)
  test('homed_auth_transaction_identity', auth_identity_test, timeout: 60,
    suite: ['nostr-homed', 'portable', 'auth', 'identity'])
endif

if get_option('domain_config').enabled()
  test('homed_domain_profile', python,
    args: [files('unit/test_domain_profile.py')], timeout: 30,
    suite: ['nostr-homed', 'portable', 'domain'])
endif

if python.found()
  test('homed_acceptance_runner_contract', python,
    args: [files('acceptance/test_acceptance_runner.py')], timeout: 30,
    suite: ['nostr-homed', 'portable', 'acceptance-contract'])
endif
