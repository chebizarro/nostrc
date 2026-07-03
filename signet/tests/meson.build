# SPDX-License-Identifier: MIT
#
# Signet tests

test_exe = executable('test_signet_smoke',
  'test_signet_smoke.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
  install: false,
)

test_replay = executable('test_replay_cache',
  'test_replay_cache.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
  install: false,
)

test_policy = executable('test_policy_engine',
  'test_policy_engine.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
  install: false,
)

test_audit = executable('test_audit_logger',
  'test_audit_logger.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
  install: false,
)

test_bootstrap_contract = executable('test_bootstrap_contract',
  'test_bootstrap_contract.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
  install: false,
)

test_capability = executable('test_capability',
  'test_capability.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
  install: false,
)

test('signet_smoke', test_exe)
test('replay_cache', test_replay)
test('policy_engine', test_policy)
test('audit_logger', test_audit)
test('bootstrap_contract', test_bootstrap_contract)
test('capability', test_capability)

if get_option('signet_enable_passkeys')
  test_store_passkeys = executable('test_store_passkeys',
    'test_store_passkeys.c',
    '../src/store_passkeys.c',
    include_directories: signet_includes + [include_directories('../src')],
    dependencies: [openssl_dep, sqlite3_dep, sodium_dep],
    install: false,
  )
  test('store_passkeys', test_store_passkeys)

  test_fido_service = executable('test_fido_service',
    'test_fido_service.c',
    '../src/fido.c',
    '../src/fido_crypto_openssl.c',
    '../src/fido_cbor.c',
    '../src/store_passkeys.c',
    include_directories: signet_includes + [include_directories('../src')],
    dependencies: [glib_dep, json_dep, openssl_dep, sqlite3_dep, sodium_dep, cbor_dep],
    install: false,
  )
  test('fido_service', test_fido_service)

  test_fido_ctaphid = executable('test_fido_ctaphid',
    'test_fido_ctaphid.c',
    link_with: signet_core,
    include_directories: signet_includes,
    dependencies: signet_deps,
    install: false,
  )
  test('fido_ctaphid', test_fido_ctaphid)

  test_fido_interop_json = executable('test_fido_interop_json',
    'test_fido_interop_json.c',
    '../src/fido.c',
    '../src/fido_crypto_openssl.c',
    '../src/fido_cbor.c',
    '../src/store_passkeys.c',
    include_directories: signet_includes + [include_directories('../src')],
    dependencies: [glib_dep, json_dep, openssl_dep, sqlite3_dep, sodium_dep, cbor_dep],
    install: false,
  )
  test('fido_interop_external', find_program('bash'),
    args: [meson.current_source_dir() / 'run_fido_interop_external_check.sh',
           test_fido_interop_json.full_path(),
           meson.current_build_dir() / 'phase3_interop'],
    timeout: 180,
  )
endif
