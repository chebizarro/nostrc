project('nostr-gobject', 'c',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 0.59.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized'
  ])

cc = meson.get_compiler('c')

# ── GLib / GObject / GIO ─────────────────────────────────────────────
glib_dep = dependency('glib-2.0', version: '>= 2.70')
gobject_dep = dependency('gobject-2.0', version: '>= 2.70')
gio_dep = dependency('gio-2.0', version: '>= 2.70')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.6', required: false)

# ── Sibling project paths (relative to nostr-gobject/) ─────────────────────
# CMake build output directory (must be built first)
cmake_build_dir = meson.current_source_dir() / '..' / 'build'

# ── libnostr (pre-built by CMake) ────────────────────────────────────
libnostr_static = cc.find_library('nostr',
  dirs: [cmake_build_dir / 'libnostr'],
  static: true)
libnostr_inc = include_directories(
  '../libnostr/include',
  '../libnostr/generated',
)
libnostr_dep = declare_dependency(
  dependencies: libnostr_static,
  include_directories: libnostr_inc)

# ── libnostr_json (Jansson backend, pre-built by CMake) ──────────────
jansson_dep = dependency('jansson')
libnostr_json_shared = cc.find_library('nostr_json',
  dirs: [cmake_build_dir / 'libjson'],
  static: false)
libnostr_json_inc = include_directories(
  '../libjson/include',
)
libnostr_json_dep = declare_dependency(
  dependencies: [libnostr_json_shared, jansson_dep],
  include_directories: libnostr_json_inc)

# ── libgo (pre-built by CMake) ───────────────────────────────────────
# liblibgo.a has channels, contexts, select, etc.
# libgo_fiber.a has the fiber/coroutine runtime
libgo_static = cc.find_library('libgo',
  dirs: [cmake_build_dir / 'libgo'],
  static: true)
libgo_fiber_static = cc.find_library('go_fiber',
  dirs: [cmake_build_dir / 'libgo'],
  static: true,
  required: false)
libgo_inc = include_directories('../libgo/include')
libgo_deps = [libgo_static]
if libgo_fiber_static.found()
  libgo_deps += libgo_fiber_static
endif
libgo_dep = declare_dependency(
  dependencies: libgo_deps,
  include_directories: libgo_inc)

# ── NIP libraries (pre-built by CMake) ───────────────────────────────
nip04_static = cc.find_library('nip04',
  dirs: [cmake_build_dir / 'nips' / 'nip04'],
  static: true)
nip04_dep = declare_dependency(
  dependencies: nip04_static,
  include_directories: include_directories('../nips/nip04/include'))

nip06_static = cc.find_library('nip06',
  dirs: [cmake_build_dir / 'nips' / 'nip06'],
  static: true)
nip06_dep = declare_dependency(
  dependencies: nip06_static,
  include_directories: include_directories('../nips/nip06/include'))

nip19_static = cc.find_library('nip19',
  dirs: [cmake_build_dir / 'nips' / 'nip19'],
  static: true)
nip19_dep = declare_dependency(
  dependencies: nip19_static,
  include_directories: include_directories('../nips/nip19/include'))

nip44_core_static = cc.find_library('nostr_nip44_core',
  dirs: [cmake_build_dir / 'nips' / 'nip44'],
  static: true)
nip44_dep = declare_dependency(
  dependencies: nip44_core_static,
  include_directories: include_directories('../nips/nip44/include'))

nip46_core_static = cc.find_library('nostr_nip46_core',
  dirs: [cmake_build_dir / 'nips' / 'nip46'],
  static: true)
nip46_dep = declare_dependency(
  dependencies: nip46_core_static,
  include_directories: include_directories('../nips/nip46/include'))

nip49_core_static = cc.find_library('nostr_nip49_core',
  dirs: [cmake_build_dir / 'nips' / 'nip49'],
  static: true,
  required: false)
nip49_glib_static = cc.find_library('nostr_nip49_glib',
  dirs: [cmake_build_dir / 'nips' / 'nip49'],
  static: true,
  required: false)
nip49_libs = []
if nip49_core_static.found()
  nip49_libs += nip49_core_static
endif
if nip49_glib_static.found()
  nip49_libs += nip49_glib_static
endif
nip49_dep = declare_dependency(
  dependencies: nip49_libs,
  include_directories: include_directories('../nips/nip49/include'))

# ── System crypto dependencies ───────────────────────────────────────
secp256k1_dep = dependency('libsecp256k1')
openssl_dep = dependency('openssl')
libsodium_dep = dependency('libsodium', required: false)
libwebsockets_dep = dependency('libwebsockets', required: true)

# nsync (no pkg-config — use find_library with explicit dirs)
nsync_lib = cc.find_library('nsync',
  dirs: ['/opt/homebrew/lib'],
  required: true)
nsync_dep = declare_dependency(
  dependencies: nsync_lib,
  include_directories: include_directories('/opt/homebrew/include', is_system: true))

# ── Version header ───────────────────────────────────────────────────
version_parts = meson.project_version().split('.')
version_conf = configuration_data()
version_conf.set('NOSTR_GOBJECT_MAJOR_VERSION', version_parts[0])
version_conf.set('NOSTR_GOBJECT_MINOR_VERSION', version_parts[1])
version_conf.set('NOSTR_GOBJECT_MICRO_VERSION', version_parts[2])
version_conf.set('NOSTR_GOBJECT_VERSION', meson.project_version())

nostr_gobject_version_h = configure_file(
  input: 'include/nostr-gobject-version.h.in',
  output: 'nostr-gobject-version.h',
  configuration: version_conf,
  install: true,
  install_dir: get_option('includedir') / 'nostr-gobject-1.0')

# ── Source files ─────────────────────────────────────────────────────
nostr_gobject_sources = files(
  'src/nostr-error.c',
  'src/nostr-enums.c',
  'src/nostr_event.c',
  'src/nostr_event_bus.c',
  'src/nostr_keys.c',
  'src/nostr_relay.c',
  'src/nostr_subscription.c',
  'src/nostr_simple_pool.c',
  'src/nostr_pool.c',
  'src/nostr_relay_store.c',
  'src/nostr_store.c',
  'src/nostr_ndb_store.c',
  'src/crypto_utils.c',
  'src/nostr_async.c',
  'src/nostr_query_batcher.c',
  'src/nostr_subscription_registry.c',
  'src/nostr_filter.c',
  'src/nostr_json.c',
  'src/nostr_pointer.c',
  'src/nostr_nip19.c',
  'src/nostr_tag_list.c',
  'src/nostr_nip46_client.c',
  'src/nostr_nip46_bunker.c',
  'src/nostr_nip49.c',
  'src/nostr_bip39.c',
)

# Header files (public API)
nostr_gobject_headers = files(
  'include/nostr-gobject.h',
  'include/nostr-error.h',
  'include/nostr-types.h',
  'include/nostr-enums.h',
  'include/nostr_event.h',
  'include/nostr_event_bus.h',
  'include/nostr_keys.h',
  'include/nostr_relay.h',
  'include/nostr_subscription.h',
  'include/nostr_simple_pool.h',
  'include/nostr_pool.h',
  'include/nostr_relay_store.h',
  'include/nostr_store.h',
  'include/crypto_utils_gobject.h',
  'include/nostr_async.h',
  'include/nostr_query_batcher.h',
  'include/nostr_subscription_registry.h',
  'include/nostr_filter.h',
  'include/nostr_json.h',
  'include/nostr_pointer.h',
  'include/nostr_nip19.h',
  'include/nostr_tag_list.h',
  'include/nostr_nip46_client.h',
  'include/nostr_nip46_bunker.h',
  'include/nostr_nip49.h',
  'include/nostr_bip39.h',
)

# ── All include directories for compilation ──────────────────────────
nostr_gobject_inc = include_directories('include')

# Collect all dependencies
nostr_gobject_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  libnostr_dep,
  libnostr_json_dep,
  libgo_dep,
  nip04_dep,
  nip06_dep,
  nip19_dep,
  nip44_dep,
  nip46_dep,
  nip49_dep,
  secp256k1_dep,
  openssl_dep,
  libwebsockets_dep,
  nsync_dep,
]

if json_glib_dep.found()
  nostr_gobject_deps += json_glib_dep
endif

if libsodium_dep.found()
  nostr_gobject_deps += libsodium_dep
endif

# ── Build the library ────────────────────────────────────────────────
# NIP-46 headers use `#include "nostr/nip46/..."` path style
nip46_inc = include_directories('../nips/nip46/include')

nostr_gobject_lib = library('nostr-gobject-1.0',
  nostr_gobject_sources,
  include_directories: [nostr_gobject_inc, nip46_inc],
  dependencies: nostr_gobject_deps,
  install: true)

# ── Declare dependency for use as subproject ─────────────────────────
nostr_gobject_dep = declare_dependency(
  link_with: nostr_gobject_lib,
  include_directories: nostr_gobject_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep])

# ── GObject Introspection ────────────────────────────────────────────
gnome = import('gnome')

if get_option('introspection')
  nostr_gobject_gir = gnome.generate_gir(nostr_gobject_lib,
    sources: nostr_gobject_sources + nostr_gobject_headers,
    namespace: 'GNostr',
    nsversion: '1.0',
    symbol_prefix: 'gnostr',
    identifier_prefix: 'GNostr',
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    install: true)
endif

# ── pkg-config ───────────────────────────────────────────────────────
pkg = import('pkgconfig')
pkg.generate(nostr_gobject_lib,
  name: 'nostr-gobject-1.0',
  description: 'GObject wrapper for libnostr — Nostr protocol library',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
  requires_private: ['openssl', 'libsecp256k1'])

# ── Header installation ──────────────────────────────────────────────
install_headers(nostr_gobject_headers, subdir: 'nostr-gobject-1.0')

# ── Tests ─────────────────────────────────────────────────────────────
if get_option('tests')
  test_nostr_event = executable('test_nostr_event',
    'tests/test_nostr_event.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_gobject_lib,
    include_directories: nostr_gobject_inc)

  test('nostr_event', test_nostr_event)

  test_nip19 = executable('test_nip19',
    'tests/test_nip19.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_gobject_lib,
    include_directories: nostr_gobject_inc)

  test('nip19', test_nip19)
endif

# ── Examples ─────────────────────────────────────────────────────────
if get_option('examples')
  connect_publish = executable('connect_publish',
    'examples/connect_publish.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_gobject_lib,
    include_directories: nostr_gobject_inc)
endif
