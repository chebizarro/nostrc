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

# nsync (no pkg-config — use find_library + find headers dynamically)
nsync_lib = cc.find_library('nsync', required: true)
nsync_inc = include_directories()
# Try common locations for nsync.h
foreach _dir : ['/opt/homebrew/include', '/usr/local/include', '/usr/include']
  if cc.has_header('nsync.h', args: '-I' + _dir)
    nsync_inc = include_directories(_dir, is_system: true)
    break
  endif
endforeach
nsync_dep = declare_dependency(
  dependencies: nsync_lib,
  include_directories: nsync_inc)

# ── Version header ───────────────────────────────────────────────────
version_parts = meson.project_version().split('.')
version_conf = configuration_data()
version_conf.set('NOSTR_GOBJECT_MAJOR_VERSION', version_parts[0])
version_conf.set('NOSTR_GOBJECT_MINOR_VERSION', version_parts[1])
version_conf.set('NOSTR_GOBJECT_MICRO_VERSION', version_parts[2])
version_conf.set('NOSTR_GOBJECT_VERSION', meson.project_version())

nostr_gobject_version_h = configure_file(
  input: 'include/nostr-gobject-1.0/nostr-gobject-version.h.in',
  output: 'nostr-gobject-version.h',
  configuration: version_conf,
  install: true,
  install_dir: get_option('includedir') / 'nostr-gobject-1.0')

# ── gnome module (needed for gdbus-codegen and GIR) ──────────────────
gnome = import('gnome')

# ── D-Bus signer proxy (gdbus-codegen) ──────────────────────────────
signer_proxy = gnome.gdbus_codegen('signer_proxy',
  sources: 'dbus/org.nostr.Signer.xml',
  namespace: 'Nostr',
  install_header: true,
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
  'src/nostr_utils.c',
  'src/nostr_profile_provider.c',
  'src/nostr_profile_service.c',
  # Moved from app (Phase 1-3 library extraction)
  'src/storage_ndb.c',
  'src/gn-timeline-query.c',
  'src/gn-nostr-profile.c',
  'src/gn-ndb-sub-dispatcher.c',
  'src/gnostr-nip10-thread-manager.c',
  'src/gnostr-thread-graph-model.c',
  'src/gnostr-thread-subscription.c',
  'src/gnostr-identity.c',
  'src/gnostr-relays.c',
  'src/gnostr-mute-list.c',
  'src/gnostr-sync-service.c',
)

# Header files (public API)
nostr_gobject_headers = files(
  'include/nostr-gobject-1.0/nostr-gobject.h',
  'include/nostr-gobject-1.0/nostr-error.h',
  'include/nostr-gobject-1.0/nostr-types.h',
  'include/nostr-gobject-1.0/nostr-enums.h',
  'include/nostr-gobject-1.0/nostr_event.h',
  'include/nostr-gobject-1.0/nostr_event_bus.h',
  'include/nostr-gobject-1.0/nostr_keys.h',
  'include/nostr-gobject-1.0/nostr_relay.h',
  'include/nostr-gobject-1.0/nostr_subscription.h',
  'include/nostr-gobject-1.0/nostr_simple_pool.h',
  'include/nostr-gobject-1.0/nostr_pool.h',
  'include/nostr-gobject-1.0/nostr_relay_store.h',
  'include/nostr-gobject-1.0/nostr_store.h',
  'include/nostr-gobject-1.0/crypto_utils_gobject.h',
  'include/nostr-gobject-1.0/nostr_async.h',
  'include/nostr-gobject-1.0/nostr_query_batcher.h',
  'include/nostr-gobject-1.0/nostr_subscription_registry.h',
  'include/nostr-gobject-1.0/nostr_filter.h',
  'include/nostr-gobject-1.0/nostr_json.h',
  'include/nostr-gobject-1.0/nostr_pointer.h',
  'include/nostr-gobject-1.0/nostr_nip19.h',
  'include/nostr-gobject-1.0/nostr_tag_list.h',
  'include/nostr-gobject-1.0/nostr_nip46_client.h',
  'include/nostr-gobject-1.0/nostr_nip46_bunker.h',
  'include/nostr-gobject-1.0/nostr_nip49.h',
  'include/nostr-gobject-1.0/nostr_bip39.h',
  'include/nostr-gobject-1.0/storage_ndb.h',
  'include/nostr-gobject-1.0/nostr_utils.h',
  'include/nostr-gobject-1.0/nostr_profile_provider.h',
  'include/nostr-gobject-1.0/nostr_profile_service.h',
  # Moved from app (Phase 1-3 library extraction)
  'include/nostr-gobject-1.0/gn-timeline-query.h',
  'include/nostr-gobject-1.0/gn-nostr-profile.h',
  'include/nostr-gobject-1.0/gn-ndb-sub-dispatcher.h',
  'include/nostr-gobject-1.0/gnostr-nip10-thread-manager.h',
  'include/nostr-gobject-1.0/gnostr-thread-graph-model.h',
  'include/nostr-gobject-1.0/gnostr-thread-subscription.h',
  'include/nostr-gobject-1.0/gnostr-identity.h',
  'include/nostr-gobject-1.0/gnostr-relays.h',
  'include/nostr-gobject-1.0/gnostr-mute-list.h',
  'include/nostr-gobject-1.0/gnostr-sync-service.h',
)

# ── All include directories for compilation ──────────────────────────
# Internal: library sources use #include "nostr_event.h" (flat)
nostr_gobject_inc = include_directories('include/nostr-gobject-1.0')
# External: consumers use #include <nostr-gobject-1.0/nostr_event.h>
nostr_gobject_pub_inc = include_directories('include')

# Transitional: app-specific headers not yet fully extracted
nostr_gobject_app_inc = include_directories(
  '../apps/gnostr/src/util',
  '../apps/gnostr/src/ipc',
  '../apps/gnostr/src/sync',
  '../apps/gnostr/src/ui',
)

# nostrdb headers (required by storage_ndb.c and ndb store)
nostr_gobject_ndb_inc = include_directories(
  '../third_party/nostrdb/src',
  '../third_party/nostrdb/ccan',
  '../third_party/nostrdb/deps/flatcc/include',
)

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
  nostr_gobject_sources + signer_proxy,
  include_directories: [nostr_gobject_inc, nip46_inc, nostr_gobject_app_inc, nostr_gobject_ndb_inc],
  dependencies: nostr_gobject_deps,
  install: true)

# ── Declare dependency for use as subproject ─────────────────────────
nostr_gobject_dep = declare_dependency(
  link_with: nostr_gobject_lib,
  include_directories: nostr_gobject_pub_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep])

# ── GObject Introspection ────────────────────────────────────────────
if get_option('introspection')
  nostr_gobject_gir = gnome.generate_gir(nostr_gobject_lib,
    sources: nostr_gobject_sources + nostr_gobject_headers,
    namespace: 'GNostr',
    nsversion: '1.0',
    symbol_prefix: 'gnostr',
    identifier_prefix: 'GNostr',
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    install: true)

  # ── VAPI generation (Vala bindings) ──────────────────────────────────
  if get_option('vapi')
    gnome.generate_vapi('nostr-gobject-1.0',
      sources: nostr_gobject_gir[0],
      packages: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
      install: true)
  endif
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
  test_env = environment()
  test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
  test_env.set('G_SLICE', 'always-malloc')

  test_nostr_event = executable('test_nostr_event',
    'tests/test_nostr_event.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_gobject_lib,
    include_directories: nostr_gobject_pub_inc)

  test('nostr_event', test_nostr_event, env: test_env)

  test_nip19 = executable('test_nip19',
    'tests/test_nip19.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_gobject_lib,
    include_directories: nostr_gobject_pub_inc)

  test('nip19', test_nip19, env: test_env)

  # ── New tests (using testkit) ──────────────────────────────────────
  # gnostr_testkit_dep should be declared via tests/testkit/meson.build
  # in the parent project. If available:
  if is_variable('gnostr_testkit_dep')
    test_store_contract = executable('test_store_contract',
      'tests/test_store_contract.c',
      dependencies: [gnostr_testkit_dep, nostr_gobject_dep])

    test('nostr-gobject:store-contract', test_store_contract, env: test_env)

    test_lifecycle_leaks = executable('test_lifecycle_leaks',
      'tests/test_lifecycle_leaks.c',
      dependencies: [gnostr_testkit_dep, nostr_gobject_dep])

    test('nostr-gobject:lifecycle-leaks', test_lifecycle_leaks, env: test_env)

    test_profile_lru = executable('test_profile_provider_lru',
      'tests/test_profile_provider_lru.c',
      dependencies: [gnostr_testkit_dep, nostr_gobject_dep])

    test('nostr-gobject:profile-provider-lru', test_profile_lru, env: test_env)

    test_latency = executable('test_main_thread_forbidden_calls',
      'tests/test_main_thread_forbidden_calls.c',
      dependencies: [gnostr_testkit_dep, nostr_gobject_dep])

    test('nostr-gobject:main-thread-latency', test_latency, env: test_env, timeout: 90)

    test_event_flow = executable('test_event_flow_ingest_subscribe',
      'tests/integ/test_event_flow_ingest_subscribe.c',
      dependencies: [gnostr_testkit_dep, nostr_gobject_dep])

    test('nostr-gobject:event-flow-integ', test_event_flow, env: test_env, timeout: 120)

    test_profile_service = executable('test_profile_service_batching',
      'tests/test_profile_service_batching.c',
      dependencies: [gnostr_testkit_dep, nostr_gobject_dep])

    test('nostr-gobject:profile-service-batching', test_profile_service, env: test_env, timeout: 90)
  endif
endif

# ── Examples ─────────────────────────────────────────────────────────
if get_option('examples')
  connect_publish = executable('connect_publish',
    'examples/connect_publish.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_gobject_lib,
    include_directories: nostr_gobject_pub_inc)
endif
