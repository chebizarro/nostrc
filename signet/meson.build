# SPDX-License-Identifier: MIT
#
# Signet - NIP-46 bunker service with SQLCipher-backed key custody
#
# Build with:
#   cd signet
#   meson setup builddir
#   meson compile -C builddir
#
# Notes:
# - This Meson build is intended for standalone development.
# - For monorepo builds, a CMake shim is provided in signet/CMakeLists.txt.
# - When using in-tree libs (libnostr/nip46) produced by the monorepo CMake build,
#   set -Dnostrc_build_dir=../build (default) and ensure those libs are present.

project('signet', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
  meson_version: '>= 0.59.0',
)

cc = meson.get_compiler('c')
fs = import('fs')

# Resolve monorepo CMake build directory for in-tree libraries
nostrc_build_dir_opt = get_option('nostrc_build_dir')
nostrc_build_dir = fs.is_absolute(nostrc_build_dir_opt) ? nostrc_build_dir_opt : (meson.current_source_dir() / nostrc_build_dir_opt)

require_in_tree = get_option('signet_require_in_tree_libs')

# Core dependencies (runtime + security)
glib_dep = dependency('glib-2.0', version: '>= 2.56', required: true)
openssl_dep = dependency('openssl', required: true)

# JSON backend
json_backend = get_option('signet_json_backend')
if json_backend == 'json-glib'
  json_dep = dependency('json-glib-1.0', required: true)
elif json_backend == 'jansson'
  json_dep = dependency('jansson', required: true)
else
  error('Unsupported signet_json_backend: ' + json_backend)
endif

# In-tree libraries (typically produced by monorepo CMake build)
# We keep directory list broad to match common CMake output layouts.
in_tree_dirs = [
  nostrc_build_dir,
  nostrc_build_dir / 'libnostr',
  nostrc_build_dir / 'libjson',
  nostrc_build_dir / 'nips' / 'nip46',
  nostrc_build_dir / 'nips' / 'nip04',
  nostrc_build_dir / 'nips' / 'nip44',
  nostrc_build_dir / 'nips' / 'nip01',
  nostrc_build_dir / 'nips' / 'nip19',
]

nostr_dep = cc.find_library('nostr', dirs: in_tree_dirs, required: require_in_tree)
nostr_json_dep = cc.find_library('nostr_json', dirs: in_tree_dirs, required: require_in_tree)
nip46_dep = cc.find_library('nip46', dirs: in_tree_dirs, required: require_in_tree)
nip19_dep = cc.find_library('nip19', dirs: in_tree_dirs, required: require_in_tree)

signet_deps = [
  glib_dep,
  json_dep,
  openssl_dep,
]

if nostr_dep.found()
  signet_deps += nostr_dep
endif
if nostr_json_dep.found()
  signet_deps += nostr_json_dep
endif
if nip46_dep.found()
  signet_deps += nip46_dep
endif
if nip19_dep.found()
  signet_deps += nip19_dep
endif

# Include paths to monorepo headers (match existing monorepo patterns)
root_inc = include_directories('..')
signet_inc = include_directories('include')
libnostr_inc = include_directories('../libnostr/include')

nips_inc = include_directories(
  '../nips/nip01/include',
  '../nips/nip04/include',
  '../nips/nip19/include',
  '../nips/nip44/include',
  '../nips/nip46/include',
)

signet_includes = [
  root_inc,
  signet_inc,
  libnostr_inc,
  nips_inc,
]

# Core library sources (everything except main entrypoints)
signet_core_sources = files(
  'src/signet_config.c',
  'src/audit_logger.c',
  'src/replay_cache.c',
  'src/key_store.c',
  'src/policy_store.c',
  'src/policy_engine.c',
  'src/relay_pool.c',
  'src/nip46_server.c',
  'src/mgmt_protocol.c',
  'src/health_server.c',
)

signet_core = static_library('signet_core',
  signet_core_sources,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: false,
)

# Daemon + CLI
executable('signetd',
  'src/signetd_main.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: true,
)

executable('signetctl',
  'src/signetctl_main.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps,
  install: true,
)

# Optional unit tests
if get_option('signet_enable_tests')
  subdir('tests')
endif

summary({
  'Health endpoint': get_option('signet_enable_health') ? 'enabled' : 'disabled',
  'JSON backend': json_backend,
  'In-tree libnostr required': require_in_tree ? 'yes' : 'no',
  'In-tree build dir': nostrc_build_dir,
}, section: 'Signet')