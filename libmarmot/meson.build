project('libmarmot', 'c',
  version: '0.11.0',
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

cc = meson.get_compiler('c')

# Dependencies
openssl_dep = dependency('openssl', required: true)
sodium_dep = dependency('libsodium', required: true)
secp256k1_dep = dependency('libsecp256k1', required: true)

# Optional: SQLite3 for persistent storage backend
sqlite3_dep = dependency('sqlite3', required: false)

# Optional: LMDB for nostrdb storage backend
lmdb_dep = dependency('lmdb', required: false)

# Sibling nostrc libraries. These are hard requirements, exactly as in
# CMakeLists.txt (which links `nostr` and `nostr_nip44_core` unconditionally):
# credentials.c, groups.c, kp_profile.c, welcome.c and messages.c include
# <nostr-event.h>/<nostr-tag.h>, and messages.c includes <nostr/nip44/nip44.h>.
# libnostr's public <nostr-tag.h> in turn includes libnostrgo's
# "string_array.h", and libnostr's tag code links libnostrgo's string_array.
# libmarmot's link closure needs no nsync, libwebsockets or GLib symbols.
nostrc_build_dir = get_option('nostrc_build_dir')
if nostrc_build_dir == ''
  # Installed siblings (pkg-config). nips/nip44 installs no .pc, so its
  # static core library is looked up next to libnostr.
  nostr_dep = dependency('nostr', required: true)
  nostrgo_dep = dependency('libnostrgo', required: true)
  nip44_lib = cc.find_library('nostr_nip44_core',
    dirs: [nostr_dep.get_variable(pkgconfig: 'libdir')],
    has_headers: ['nostr/nip44/nip44.h'],
    header_dependencies: [nostrgo_dep],
    required: false,
  )
  if not nip44_lib.found()
    error('libmarmot requires the NIP-44 core library (libnostr_nip44_core) ' +
      'and <nostr/nip44/nip44.h> next to the installed libnostr (' +
      nostr_dep.get_variable(pkgconfig: 'libdir') + '). nips/nip44 currently ' +
      'installs only its headers; either install the library there or ' +
      'configure with -Dnostrc_build_dir=<built nostrc CMake tree>.')
  endif
  nip44_dep = declare_dependency(dependencies: nip44_lib)
else
  # Siblings from an already-built nostrc CMake tree (headers from this
  # checkout), the same fallback marmot-gobject uses for libmarmot.
  nostrc_src_dir = meson.current_source_dir() / '..'
  nostr_dep = declare_dependency(
    dependencies: cc.find_library('nostr',
      dirs: [nostrc_build_dir / 'libnostr'], required: true),
    compile_args: ['-I' + nostrc_src_dir / 'libnostr' / 'include'],
  )
  nostrgo_dep = declare_dependency(
    dependencies: cc.find_library('nostrgo',
      dirs: [nostrc_build_dir / 'libgo'], required: true),
    compile_args: ['-I' + nostrc_src_dir / 'libgo' / 'include'],
  )
  nip44_dep = declare_dependency(
    dependencies: cc.find_library('nostr_nip44_core',
      dirs: [nostrc_build_dir / 'nips' / 'nip44'], required: true),
    compile_args: ['-I' + nostrc_src_dir / 'nips' / 'nip44' / 'include'],
  )
endif

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
  'src/mls/mls_app_data_update.c',
  'src/mls/mls_key_package.c',
  'src/mls/mls_group.c',
  'src/mls/mls_welcome.c',
  'src/credentials.c',
  'src/kp_profile.c',
  'src/commits.c',
  'src/groups.c',
  'src/welcome.c',
  'src/messages.c',
  'src/media.c',
  'src/storage_nostrdb.c',
)

# Conditionally add SQLite backend
if sqlite3_dep.found()
  marmot_sources += files('src/storage_sqlite.c')
endif

# Public include directories
marmot_inc = include_directories('include')
marmot_src_inc = include_directories('src')

# Gather dependencies. Users precede providers (NIP-44, libnostr, libnostrgo,
# then crypto) for single-pass static linkers.
marmot_deps = [nip44_dep, nostr_dep, nostrgo_dep, openssl_dep, sodium_dep,
  secp256k1_dep]
marmot_c_args = []
if sqlite3_dep.found()
  marmot_deps += sqlite3_dep
  marmot_c_args += '-DMARMOT_HAVE_SQLITE=1'
endif
if lmdb_dep.found()
  marmot_deps += lmdb_dep
endif
if get_option('adopted_key_package_producer')
  marmot_c_args += '-DMARMOT_ENABLE_ADOPTED_KEY_PACKAGE_PRODUCER=1'
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

# Tests: the same executables and test names as CMakeLists.txt.
if get_option('tests')
  marmot_test_names = [
    'test_mls_tls',
    'test_mls_crypto',
    'test_extension',
    'test_storage',
    'test_types',
    'test_mls_tree',
    'test_mls_key_schedule',
    'test_mls_key_schedule_vectors',
    'test_mls_framing',
    'test_mls_key_package',
    'test_mls_group',
    'test_app_data_update',
    'test_mls_welcome',
    'test_protocol',
    'test_commits',
    'test_ratchet_persist',
    'test_kp_profile',
    'test_storage_contract',
    'test_media',
    'test_mls_tree_vectors',
    'test_rfc9420_vectors',
  ]
  foreach name : marmot_test_names
    test('marmot_' + name, executable(name, 'tests' / (name + '.c'),
      include_directories: [marmot_inc, marmot_src_inc],
      link_with: libmarmot,
      dependencies: marmot_deps,
    ))
  endforeach

  # MDK interoperability. The suite locates tests/vectors/mdk relative to its
  # working directory; pin it so out-of-tree builds do not silently skip the
  # MDK vectors.
  test('marmot_test_interop', executable('test_marmot_interop',
      'tests/test_interop.c',
      'tests/mdk_vector_loader.c',
      'tests/mdk_vector_stubs.c',
      include_directories: [marmot_inc, marmot_src_inc,
        include_directories('tests')],
      link_with: libmarmot,
      dependencies: marmot_deps,
    ),
    workdir: meson.current_source_dir(),
  )
endif
