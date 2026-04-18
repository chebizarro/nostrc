
# Unit tests — mirrors CMakeLists.txt test set

test_manifest = executable('test_manifest',
  'unit/test_manifest.c',
  '../src/common/nostr_manifest.c',
  '../src/common/util.c',
  include_directories: inc,
  dependencies: [jann])
test('test_manifest', test_manifest, timeout: 30)

test_cache = executable('test_cache',
  'unit/test_cache.c',
  '../src/common/nostr_cache.c',
  '../src/common/util.c',
  include_directories: inc,
  dependencies: [sqlite3, openssl])
test('test_cache', test_cache, timeout: 30)

test_cache_config = executable('test_cache_config',
  'unit/test_cache_config.c',
  '../src/common/nostr_cache.c',
  '../src/common/util.c',
  include_directories: inc,
  dependencies: [sqlite3, openssl])
test('test_cache_config', test_cache_config, timeout: 30)

test_uid_map = executable('test_uid_map',
  'unit/test_uid_map.c',
  '../src/common/nostr_cache.c',
  '../src/common/util.c',
  include_directories: inc,
  dependencies: [sqlite3, openssl])
test('test_uid_map', test_uid_map, timeout: 30)

test_groups = executable('test_groups',
  'unit/test_groups.c',
  '../src/common/nostr_cache.c',
  '../src/common/util.c',
  include_directories: inc,
  dependencies: [sqlite3, openssl])
test('test_groups', test_groups, timeout: 30)

# End-to-end integration test (requires FUSE, dbus-run-session, python3+websockets)
e2e_script = files('integration/run_e2e_real_signer_test.sh')
test('e2e_real_signer', find_program('bash'), args: [e2e_script],
     timeout: 120, is_parallel: false)
