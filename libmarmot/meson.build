project('libmarmot', 'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

marmot_version = meson.project_version()
marmot_version_parts = marmot_version.split('.')

# Generate version header
version_conf = configuration_data()
version_conf.set('MARMOT_VERSION_MAJOR', marmot_version_parts[0])
version_conf.set('MARMOT_VERSION_MINOR', marmot_version_parts[1])
version_conf.set('MARMOT_VERSION_PATCH', marmot_version_parts[2])
version_conf.set('MARMOT_VERSION_STRING', marmot_version)

marmot_version_h = configure_file(
  input: 'include/marmot/marmot-version.h.in',
  output: 'marmot-version.h',
  configuration: version_conf,
  install_dir: get_option('includedir') / 'marmot',
)

# Dependencies
openssl_dep = dependency('openssl', required: true)
sodium_dep = dependency('libsodium', required: true)
secp256k1_dep = dependency('libsecp256k1', required: true)

# Optional: SQLite3 for persistent storage backend
sqlite3_dep = dependency('sqlite3', required: false)

# Optional: LMDB for nostrdb storage backend
lmdb_dep = dependency('lmdb', required: false)

# Sibling libraries (available when built as part of nostrc)
nostr_dep = dependency('nostr', required: false)
nip44_dep = dependency('nip44', required: false)

# Sources
marmot_sources = files(
  'src/marmot.c',
  'src/marmot_error.c',
  'src/marmot_types.c',
  'src/marmot_storage.c',
  'src/extension.c',
  'src/storage_memory.c',
  'src/mls/mls_crypto.c',
  'src/mls/mls_tls.c',
  'src/mls/mls_tree.c',
  'src/mls/mls_key_schedule.c',
  'src/mls/mls_framing.c',
  'src/mls/mls_key_package.c',
  'src/mls/mls_group.c',
  'src/mls/mls_welcome.c',
  'src/credentials.c',
  'src/groups.c',
  'src/welcome.c',
  'src/messages.c',
  'src/media.c',
  'src/storage_nostrdb.c',
)

# Conditionally add SQLite backend
if sqlite3_dep.found()
  marmot_sources += files('src/storage_sqlite.c')
endif()

# Public include directories
marmot_inc = include_directories('include')
marmot_src_inc = include_directories('src')

# Gather optional dependencies
marmot_deps = [openssl_dep, sodium_dep, secp256k1_dep, nostr_dep, nip44_dep]
marmot_c_args = []
if sqlite3_dep.found()
  marmot_deps += sqlite3_dep
  marmot_c_args += '-DMARMOT_HAVE_SQLITE=1'
endif
if lmdb_dep.found()
  marmot_deps += lmdb_dep
endif

# Library
libmarmot = library('marmot',
  marmot_sources,
  include_directories: [marmot_inc, marmot_src_inc],
  c_args: marmot_c_args,
  dependencies: marmot_deps,
  install: true,
  version: marmot_version,
  soversion: marmot_version_parts[0],
)

# Dependency object for consumers
marmot_dep = declare_dependency(
  include_directories: marmot_inc,
  link_with: libmarmot,
  dependencies: [openssl_dep, sodium_dep, secp256k1_dep],
)

# Install public headers
install_headers(
  'include/marmot/marmot.h',
  'include/marmot/marmot-error.h',
  'include/marmot/marmot-types.h',
  'include/marmot/marmot-storage.h',
  subdir: 'marmot',
)

# pkg-config
pkg = import('pkgconfig')
pkg.generate(libmarmot,
  name: 'libmarmot',
  description: 'C implementation of the Marmot protocol (MLS + Nostr)',
  requires: ['libsodium', 'openssl', 'libsecp256k1'],
)

# Tests
if get_option('tests')
  test_mls_tls = executable('test_mls_tls',
    'tests/test_mls_tls.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_tls_codec', test_mls_tls)

  test_mls_crypto = executable('test_mls_crypto',
    'tests/test_mls_crypto.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_crypto', test_mls_crypto)

  test_extension = executable('test_extension',
    'tests/test_extension.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_extension', test_extension)

  test_storage = executable('test_storage',
    'tests/test_storage.c',
    include_directories: [marmot_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_storage', test_storage)

  test_types = executable('test_types',
    'tests/test_types.c',
    include_directories: [marmot_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_types', test_types)

  test_mls_tree = executable('test_mls_tree',
    'tests/test_mls_tree.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_tree', test_mls_tree)

  test_mls_key_schedule = executable('test_mls_key_schedule',
    'tests/test_mls_key_schedule.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_key_schedule', test_mls_key_schedule)

  test_mls_framing = executable('test_mls_framing',
    'tests/test_mls_framing.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_framing', test_mls_framing)

  test_mls_key_package = executable('test_mls_key_package',
    'tests/test_mls_key_package.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_key_package', test_mls_key_package)

  test_mls_group = executable('test_mls_group',
    'tests/test_mls_group.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_group', test_mls_group)

  test_mls_welcome = executable('test_mls_welcome',
    'tests/test_mls_welcome.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_mls_welcome', test_mls_welcome)

  test_protocol = executable('test_protocol',
    'tests/test_protocol.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep, secp256k1_dep, nostr_dep, nip44_dep],
  )
  test('marmot_protocol', test_protocol)

  # Storage contract tests (all backends)
  test_storage_contract = executable('test_storage_contract',
    'tests/test_storage_contract.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: marmot_deps,
  )
  test('marmot_storage_contract', test_storage_contract)

  # MIP-04 media encryption tests
  test_media = executable('test_media',
    'tests/test_media.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_media', test_media)

  # RFC 9420 test vectors & crypto validation
  test_rfc9420 = executable('test_rfc9420_vectors',
    'tests/test_rfc9420_vectors.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep],
  )
  test('marmot_rfc9420_vectors', test_rfc9420)

  # MDK interoperability tests
  test_interop = executable('test_interop',
    'tests/test_interop.c',
    include_directories: [marmot_inc, marmot_src_inc],
    link_with: libmarmot,
    dependencies: [openssl_dep, sodium_dep, secp256k1_dep],
  )
  test('marmot_interop', test_interop)
endif
