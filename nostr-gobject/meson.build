project('nostr-gobject', 'c',
  version: '3.0.1',
  license: 'MIT',
  meson_version: '>= 0.59.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized'
  ])

cc = meson.get_compiler('c')
export_conf = configuration_data()
export_conf.set('API', 'NOSTR_GOBJECT_API')
export_conf.set('STATIC', get_option('default_library') == 'static' ? 1 : 0)
export_conf.set('STATIC_DEFINE', 'NOSTR_GOBJECT_STATIC_DEFINE')
export_conf.set('EXPORTS', 'nostr_gobject_EXPORTS')
configure_file(input: '../cmake/meson-export.h.in',
  output: 'nostr-gobject-export.h', configuration: export_conf,
  install_dir: get_option('includedir') / 'nostr-gobject-1.0')
fs = import('fs')

# ── GLib / GObject / GIO ─────────────────────────────────────────────
glib_dep = dependency('glib-2.0', version: '>= 2.70')
gobject_dep = dependency('gobject-2.0', version: '>= 2.70')
gio_dep = dependency('gio-2.0', version: '>= 2.70')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.6', required: false)

# ── Sibling project paths (relative to nostr-gobject/) ─────────────────────
# CMake build output directory (must be built first)
cmake_build_dir = get_option('cmake_build_dir')
if cmake_build_dir == ''
  cmake_build_dir = fs.parent(meson.current_source_dir()) / 'build'
endif
# Bind the core to the real artifacts of this CMake tree (libnostr.a,
# libnostr-json, libnostrgo.a). find_library() would silently fall back to a
# similarly named system library when the tree lacks them.
json_suffix = host_machine.system() == 'darwin' ? '.dylib' : '.so'
core_artifact = cmake_build_dir / 'libnostr' / 'libnostr.a'
json_artifact = cmake_build_dir / 'libjson' / ('libnostr-json' + json_suffix)
go_artifact = cmake_build_dir / 'libgo' / 'libnostrgo.a'
foreach artifact : [core_artifact, json_artifact, go_artifact]
  if not fs.is_file(artifact)
    error('Required CMake artifact missing: ' + artifact + '; build the CMake static core first')
  endif
endforeach

# ── libnostr (pre-built by CMake) ────────────────────────────────────
libnostr_static = declare_dependency(link_args: [core_artifact])
libnostr_inc = include_directories(
  '../libnostr/include',
  '../libnostr/generated',
  cmake_build_dir / 'libnostr/include',
  cmake_build_dir / 'libjson/include',
)
libnostr_dep = declare_dependency(
  dependencies: libnostr_static,
  include_directories: libnostr_inc)

# ── libnostr_json (Jansson backend, pre-built by CMake) ──────────────
jansson_dep = dependency('jansson')
libnostr_json_shared = declare_dependency(link_args: [json_artifact])
libnostr_json_inc = include_directories(
  '../libjson/include',
)
libnostr_json_dep = declare_dependency(
  dependencies: [libnostr_json_shared, jansson_dep],
  include_directories: libnostr_json_inc)

# ── libgo (pre-built by CMake) ───────────────────────────────────────
# libnostrgo.a has channels, contexts, select, etc.
# libgo_fiber.a has the fiber/coroutine runtime
libgo_static = declare_dependency(link_args: [go_artifact])
libgo_fiber_static = cc.find_library('go_fiber',
  dirs: [cmake_build_dir / 'libgo'],
  static: true,
  required: false)
libgo_inc = include_directories('../libgo/include', cmake_build_dir / 'libgo/include')
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
  include_directories: include_directories('../nips/nip04/include', cmake_build_dir / 'nips/nip04/include'))

nip06_static = cc.find_library('nip06',
  dirs: [cmake_build_dir / 'nips' / 'nip06'],
  static: true)
nip06_dep = declare_dependency(
  dependencies: nip06_static,
  include_directories: include_directories('../nips/nip06/include', cmake_build_dir / 'nips/nip06/include'))

# The installed relay header declares the NIP-11 API unconditionally.
nip11_artifact = cmake_build_dir / 'nips/nip11/libnip11.a'
if not fs.is_file(nip11_artifact)
  error('Required CMake NIP-11 artifact missing: ' + nip11_artifact)
endif
nip11_dep = declare_dependency(
  link_args: [nip11_artifact],
  include_directories: include_directories('../nips/nip11/include', cmake_build_dir / 'nips/nip11/include'))

nip19_static = cc.find_library('nip19',
  dirs: [cmake_build_dir / 'nips' / 'nip19'],
  static: true)
nip19_dep = declare_dependency(
  dependencies: nip19_static,
  include_directories: include_directories('../nips/nip19/include', cmake_build_dir / 'nips/nip19/include'))

nip44_core_static = cc.find_library('nostr_nip44_core',
  dirs: [cmake_build_dir / 'nips' / 'nip44'],
  static: true)
nip44_dep = declare_dependency(
  dependencies: nip44_core_static,
  include_directories: include_directories('../nips/nip44/include', cmake_build_dir / 'nips/nip44/include'))

nip46_core_static = cc.find_library('nostr_nip46_core',
  dirs: [cmake_build_dir / 'nips' / 'nip46'],
  static: true)
nip46_dep = declare_dependency(
  dependencies: nip46_core_static,
  include_directories: include_directories('../nips/nip46/include', cmake_build_dir / 'nips/nip46/include'))

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
nip49_includes = [include_directories('../nips/nip49/include')]
if fs.is_dir(cmake_build_dir / 'nips/nip49/include')
  nip49_includes += [include_directories(cmake_build_dir / 'nips/nip49/include')]
endif
nip49_dep = declare_dependency(
  dependencies: nip49_libs,
  include_directories: nip49_includes)

# ── System crypto dependencies ───────────────────────────────────────
secp256k1_dep = dependency('libsecp256k1')
openssl_dep = dependency('openssl')
libsodium_dep = dependency('libsodium', required: false)
libwebsockets_dep = dependency('libwebsockets', required: true)

# nsync (no pkg-config — use find_library + compiler header search)
nsync_lib = cc.find_library('nsync', required: true)
# Let the compiler find nsync.h via its default search paths (works on
# Linux, macOS Homebrew ARM & Intel, MacPorts, and custom prefixes set
# via CFLAGS/C_INCLUDE_PATH).
if cc.has_header('nsync.h')
  nsync_dep = declare_dependency(dependencies: nsync_lib)
else
  # Fallback: check platform-specific prefix dirs without hard-coding paths.
  # get_option('prefix') and common system dirs are already on the search path,
  # so this should only trigger in unusual setups.
  error('nsync.h not found. Install nsync or set C_INCLUDE_PATH to its include dir.')
endif

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
# The service's own interface file is the only copy (nostrc-56id).
signer_proxy = gnome.gdbus_codegen('signer_proxy',
  sources: files('../nips/nip55l/dbus/org.nostr.Signer.xml'),
  namespace: 'Nostr',
  extra_args: ['--symbol-decorator', 'NOSTR_GOBJECT_API',
               '--symbol-decorator-header', 'nostr-gobject-export.h'],
  install_header: true,
  install_dir: get_option('includedir') / 'nostr-gobject-1.0')

# ── Source files ─────────────────────────────────────────────────────
nostr_gobject_sources = files(
  'src/gn-blossom-client.c',
  'src/gn-vault.c',
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
  'src/gnostr-signer-bridge.c',
  'src/gnostr-app-bridge.c',
)

# Header files (public API)
nostr_gobject_headers = files(
  'include/nostr-gobject-1.0/gn-blossom-client.h',
  'include/nostr-gobject-1.0/gn-vault.h',
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
  'include/nostr-gobject-1.0/gnostr-signer-bridge.h',
  'include/nostr-gobject-1.0/gnostr-app-bridge.h',
)

# ── All include directories for compilation ──────────────────────────
# Internal: library sources use #include "nostr_event.h" (flat)
nostr_gobject_inc = include_directories('include/nostr-gobject-1.0')
# External: consumers use #include <nostr-gobject-1.0/nostr_event.h>
nostr_gobject_pub_inc = include_directories('include')

# nostrdb headers (required by storage_ndb.c and ndb store)
nostr_gobject_ndb_inc = include_directories(
  '../third_party/nostrdb/src',
  '../third_party/nostrdb/ccan',
  '../third_party/nostrdb/deps/flatcc/include',
)

# Collect all dependencies
# Match CMake's actual archive closure without forcing system crypto/HTTP
# dependencies to static linkage too.
blossom_dep = dependency('blossom', version: '>= 0.2.0')
vault_dep = dependency('vault', version: '>= 0.1.0')
bud02_dep = dependency('bud02')
curl_dep = dependency('libcurl')
storage_artifacts = []
foreach component : [['components/nostrdb', 'nostrdb_storage'],
                     ['libnostr', 'nostrdb'],
                     ['components/nostrdb', 'nostrdb_vendored'],
                     ['nips/nip59', 'nostr_nip59_core']]
  artifact = cmake_build_dir / component[0] / ('lib' + component[1] + '.a')
  if not fs.is_file(artifact)
    error('Required CMake storage/vault artifact missing: ' + artifact)
  endif
  storage_artifacts += artifact
endforeach
storage_archive_dep = declare_dependency(link_args: storage_artifacts)
nostr_gobject_deps = [
  blossom_dep,
  vault_dep,
  glib_dep,
  gobject_dep,
  gio_dep,
  libnostr_dep,
  storage_archive_dep,
  bud02_dep,
  curl_dep,
  libnostr_json_dep,
  libgo_dep,
  nip04_dep,
  nip06_dep,
  nip11_dep,
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

# Exact archive closure shared by the final consumer, pkg-config and GIR.
core_archives = [core_artifact, go_artifact, nip11_artifact] + storage_artifacts
foreach component : [['libgo', 'go_fiber'], ['nips/nip04', 'nip04'],
                     ['nips/nip06', 'nip06'], ['nips/nip19', 'nip19'],
                     ['nips/nip44', 'nostr_nip44_core'],
                     ['nips/nip46', 'nostr_nip46_core'],
                     ['nips/nip49', 'nostr_nip49_core'],
                     ['nips/nip49', 'nostr_nip49_glib']]
  artifact = cmake_build_dir / component[0] / ('lib' + component[1] + '.a')
  if fs.is_file(artifact)
    core_archives += artifact
  endif
endforeach

# ── Build the library ────────────────────────────────────────────────
# NIP-46 headers use `#include "nostr/nip46/..."` path style
nip46_inc = include_directories('../nips/nip46/include')

nostr_gobject_c_args = ['-Dnostr_gobject_EXPORTS', '-DENABLE_NIP11']
# On ELF a shared library follows NOSTRC_ARCHIVES_FROM_EXECUTABLE in the root
# CMake build: one static core per process, supplied by the final executable,
# not a hidden second copy inside this DSO. A static library also leaves the
# closure to its consumer; Mach-O and DLL builds embed the core, as in CMake.
exe_owned_core = (get_option('default_library') != 'static' and
  host_machine.system() not in ['darwin', 'windows', 'cygwin'])
consumer_closure = exe_owned_core or get_option('default_library') == 'static'
nostr_gobject_link_deps = nostr_gobject_deps
nostr_gobject_link_options = []
if exe_owned_core
  nostr_gobject_link_deps = []
  foreach dep : nostr_gobject_deps
    nostr_gobject_link_deps += dep.partial_dependency(compile_args: true, includes: true)
  endforeach
  nostr_gobject_link_deps += [glib_dep, gobject_dep, gio_dep]
  nostr_gobject_link_options = ['b_lundef=false']
endif
nostr_gobject_lib = library('nostr-gobject-1.0',
  nostr_gobject_sources + signer_proxy,
  include_directories: [nostr_gobject_inc, nostr_gobject_pub_inc, nip46_inc, nostr_gobject_ndb_inc],
  dependencies: nostr_gobject_link_deps,
  override_options: nostr_gobject_link_options,
  version: meson.project_version(),
  soversion: '3',
  gnu_symbol_visibility: 'hidden',
  c_args: nostr_gobject_c_args,
  link_args: cc.get_supported_link_arguments('-Wl,--exclude-libs,ALL'),
  install: true)

# ── Declare dependency for use as subproject ─────────────────────────
consumer_deps = nostr_gobject_deps
if not consumer_closure
  # The DSO embeds the core; consumers need only its compile interface.
  consumer_deps = [glib_dep, gobject_dep, gio_dep, vault_dep]
  foreach dep : nostr_gobject_deps
    consumer_deps += dep.partial_dependency(compile_args: true, includes: true)
  endforeach
endif
nostr_gobject_dep = declare_dependency(
  link_with: nostr_gobject_lib,
  include_directories: [nostr_gobject_pub_inc, include_directories('.')],
  dependencies: consumer_deps)

# ── GObject Introspection ────────────────────────────────────────────
if get_option('introspection')
  gir_env = environment()
  if exe_owned_core
    # Meson's GIR adapter drops absolute archive link arguments. Supply the
    # same real executable-owned closure to the scanner's strict final link.
    gir_env.prepend('LDFLAGS', ' '.join(['-Wl,--start-group'] + core_archives +
      [json_artifact, '-lvault', '-lporthome', '-lblossom', '-lbud02', '-Wl,--end-group']), separator: ' ')
    gir_env.prepend('LD_LIBRARY_PATH', cmake_build_dir / 'libjson')
  endif
  nostr_gobject_gir = gnome.generate_gir(nostr_gobject_lib,
    sources: nostr_gobject_sources + nostr_gobject_headers,
    dependencies: nostr_gobject_deps,
    env: gir_env,
    namespace: 'GNostr',
    nsversion: '1.0',
    symbol_prefix: ['gnostr', 'gn'],
    identifier_prefix: ['GNostr', 'Gnostr', 'Gn'],
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
  libraries: consumer_closure ? core_archives + [json_artifact, nsync_lib] : [],
  requires: [glib_dep, gobject_dep, gio_dep, blossom_dep, vault_dep, bud02_dep,
             curl_dep, secp256k1_dep, openssl_dep, libwebsockets_dep] +
            (json_glib_dep.found() ? [json_glib_dep] : []) +
            (libsodium_dep.found() ? [libsodium_dep] : []),
  requires_private: ['openssl', 'libsecp256k1'])

# ── Header installation ──────────────────────────────────────────────
install_headers(nostr_gobject_headers, subdir: 'nostr-gobject-1.0')

# ── Tests ─────────────────────────────────────────────────────────────
if get_option('tests')
  test_env = environment()
  test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
  test_env.set('G_SLICE', 'always-malloc')
  test_env.prepend('LD_LIBRARY_PATH', cmake_build_dir / 'libjson')

  test_nostr_event = executable('test_nostr_event',
    'tests/test_nostr_event.c',
    dependencies: nostr_gobject_dep,
    include_directories: nostr_gobject_pub_inc)

  test('nostr_event', test_nostr_event, env: test_env)

  test_nip19 = executable('test_nip19',
    'tests/test_nip19.c',
    dependencies: nostr_gobject_dep,
    include_directories: nostr_gobject_pub_inc)

  test('nip19', test_nip19, env: test_env)

  test_vault = executable('test_vault', 'tests/test_vault.c',
    dependencies: [nostr_gobject_dep, libnostr_dep, libnostr_json_dep])
  test('nostr-gobject:vault', test_vault, env: test_env, timeout: 30)

  test_blossom = executable('test_blossom_client', 'tests/test_blossom_client.c',
    dependencies: [nostr_gobject_dep, libnostr_dep, dependency('bud02'), dependency('libsoup-3.0')])
  test('nostr-gobject:blossom', test_blossom, env: test_env, timeout: 30)

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
    dependencies: nostr_gobject_dep,
    include_directories: nostr_gobject_pub_inc)
endif
