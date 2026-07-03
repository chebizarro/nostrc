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
gio_dep = dependency('gio-2.0', required: true)
openssl_dep = dependency('openssl', required: true)
sqlite3_dep = dependency('sqlite3', required: true)
sodium_dep = dependency('libsodium', required: true)
mhd_dep = dependency('libmicrohttpd', required: true)
cbor_dep = dependency('libcbor', required: get_option('signet_enable_passkeys'))

# nsync is pulled in transitively via libgo headers (context.h -> nsync.h) and
# symbols. It ships no pkg-config file, so mirror the CMake find_path/find_library
# approach: locate the header across common prefixes and link the lib when present.
nsync_deps = []
nsync_lib = cc.find_library('nsync', required: false)
if nsync_lib.found()
  nsync_deps += nsync_lib
endif
nsync_cargs = []
foreach d : ['/opt/homebrew/include', '/usr/local/include', '/opt/local/include']
  if fs.exists(d / 'nsync.h')
    nsync_cargs += '-I' + d
  endif
endforeach
nsync_dep = declare_dependency(compile_args: nsync_cargs, dependencies: nsync_deps)

# JSON backend
json_backend = get_option('signet_json_backend')
if json_backend == 'json-glib'
  json_dep = dependency('json-glib-1.0', required: true)
elif json_backend == 'jansson'
  json_dep = dependency('jansson', required: true)
else
  error('Unsupported signet_json_backend: ' + json_backend)
endif

# In-tree libraries produced by the monorepo CMake build (under nostrc_build_dir).
# The standalone meson build links these prebuilt archives directly so it can
# produce a working signetd. The set, names, and order mirror the CMake link
# line (see bd nostrc-1fb); build the monorepo CMake target first.
in_tree_archives = [
  'nips/nip19/libnip19.a',
  'nips/nip46/libnostr_nip46_core.a',
  'nips/nip04/libnip04.a',
  'nips/nip44/libnostr_nip44_core.a',
  'nips/nip11/libnip11.a',
  'libnostr/libnostr.a',
  'libgo/liblibgo.a',
  'libnostr/libnostrdb.a',
]
in_tree_link_args = []
in_tree_found = true
foreach rel : in_tree_archives
  ap = nostrc_build_dir / rel
  if fs.exists(ap)
    in_tree_link_args += ap
  else
    in_tree_found = false
  endif
endforeach

# nostr_json is built as a shared library in the CMake tree.
nostr_json_dir = nostrc_build_dir / 'libjson'
if fs.exists(nostr_json_dir / 'libnostr_json.dylib') or fs.exists(nostr_json_dir / 'libnostr_json.so')
  in_tree_link_args += ['-L' + nostr_json_dir, '-lnostr_json', '-Wl,-rpath,' + nostr_json_dir]
else
  in_tree_found = false
endif

# flatccrt is a STATIC archive that libnostrdb depends on, so it must come after
# the archives above in the link line — add it as a positional link arg.
extra_lib_dirs = ['/opt/homebrew/lib', '/usr/local/lib', '/opt/local/lib']
foreach d : extra_lib_dirs
  if fs.exists(d / 'libflatccrt.a')
    in_tree_link_args += d / 'libflatccrt.a'
  endif
endforeach

if require_in_tree and not in_tree_found
  error('signet_require_in_tree_libs=true but prebuilt monorepo libraries were not found under ' + nostrc_build_dir + '. Build the CMake target first, or set -Dsignet_require_in_tree_libs=false.')
endif

# External libs the monorepo stack pulls in (dylibs -> order-independent).
in_tree_ext_deps = []
secp_dep = dependency('libsecp256k1', required: false)
if not secp_dep.found()
  secp_dep = cc.find_library('secp256k1', dirs: extra_lib_dirs, required: false)
endif
if secp_dep.found()
  in_tree_ext_deps += secp_dep
endif
ws_dep = dependency('libwebsockets', required: false)
if not ws_dep.found()
  ws_dep = cc.find_library('websockets', dirs: extra_lib_dirs, required: false)
endif
if ws_dep.found()
  in_tree_ext_deps += ws_dep
endif
threads_dep = dependency('threads', required: false)
if threads_dep.found()
  in_tree_ext_deps += threads_dep
endif
if host_machine.system() == 'darwin'
  security_dep = dependency('appleframeworks', modules: ['Security'], required: false)
  if security_dep.found()
    in_tree_ext_deps += security_dep
  endif
endif
# nsync ships as a dylib in a non-default prefix; link it explicitly (the earlier
# nsync_dep only provides the include path).
nsync_link = cc.find_library('nsync', dirs: extra_lib_dirs, required: false)
if nsync_link.found()
  in_tree_ext_deps += nsync_link
endif
# libcurl is pulled in transitively by libnostr (NIP-05/NIP-11 fetch).
curl_dep = dependency('libcurl', required: false)
if not curl_dep.found()
  curl_dep = cc.find_library('curl', dirs: extra_lib_dirs, required: false)
endif
if curl_dep.found()
  in_tree_ext_deps += curl_dep
endif

# Single dependency carrying the in-tree archive link line + external libs.
in_tree_dep = declare_dependency(link_args: in_tree_link_args, dependencies: in_tree_ext_deps)

signet_deps = [
  glib_dep,
  gio_dep,
  json_dep,
  openssl_dep,
  sqlite3_dep,
  sodium_dep,
  mhd_dep,
  nsync_dep,
]
if get_option('signet_enable_passkeys')
  signet_deps += cbor_dep
endif

# Include paths to monorepo headers (match existing monorepo patterns)
root_inc = include_directories('..')
signet_inc = include_directories('include')
libnostr_inc = include_directories('../libnostr/include')
# libnostr public headers (e.g. nostr-tag.h) include libgo headers such as
# string_array.h; the standalone build needs libgo/include on the path.
libgo_inc = include_directories('../libgo/include')

nips_inc = include_directories(
  '../nips/nip01/include',
  '../nips/nip04/include',
  '../nips/nip17/include',
  '../nips/nip19/include',
  '../nips/nip44/include',
  '../nips/nip46/include',
  '../nips/nip11/include',
)

signet_includes = [
  root_inc,
  signet_inc,
  libnostr_inc,
  libgo_inc,
  nips_inc,
]

# Core library sources (everything except main entrypoints)
signet_core_sources = files(
  'src/signet_config.c',
  'src/store.c',
  'src/audit_logger.c',
  'src/replay_cache.c',
  'src/key_store.c',
  'src/policy_store.c',
  'src/policy_engine.c',
  'src/relay_pool.c',
  'src/nip46_server.c',
  'src/mgmt_protocol.c',
  'src/health_server.c',
  'src/store_tokens.c',
  'src/store_secrets.c',
  'src/store_leases.c',
  'src/store_audit.c',
  'src/nostr_auth.c',
  'src/capability.c',
  'src/bootstrap_server.c',
  'src/revocation.c',
  'src/dbus_common.c',
  'src/dbus_unix.c',
  'src/bootstrap_delivery.c',
  'src/dbus_tcp.c',
  'src/nip5l_transport.c',
  'src/session_broker.c',
  'src/ssh_agent.c',
)

# Experimental FIDO2/passkeys modules.
# See docs/plans/signet-passkeys-fido-2026-07-02.md. Off by default; requires
# OpenSSL plus libcbor for CTAP2/WebAuthn CBOR/COSE handling.
if get_option('signet_enable_passkeys')
  signet_core_sources += files(
    'src/fido_crypto_openssl.c',
    'src/fido_cbor.c',
    'src/store_passkeys.c',
    'src/fido.c',
    'src/fido_ctaphid.c',
  )
  add_project_arguments('-DSIGNET_ENABLE_PASSKEYS=1', language: 'c')
endif

signetd_cargs = []
if get_option('signet_enable_test_hooks')
  signetd_cargs += '-DSIGNET_ENABLE_TEST_HOOKS=1'
endif

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
  dependencies: signet_deps + [in_tree_dep],
  c_args: signetd_cargs,
  install: true,
)

executable('signetctl',
  'src/signetctl_main.c',
  link_with: signet_core,
  include_directories: signet_includes,
  dependencies: signet_deps + [in_tree_dep],
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
  'libsodium': sodium_dep.found() ? 'found' : 'not found',
  'libcbor': cbor_dep.found() ? 'found' : 'not found',
  'Passkeys': get_option('signet_enable_passkeys') ? 'enabled' : 'disabled',
  'Test hooks': get_option('signet_enable_test_hooks') ? 'enabled' : 'disabled',
}, section: 'Signet')
