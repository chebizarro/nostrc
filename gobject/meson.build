project('nostr-glib', 'c',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 0.59.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized'
  ])

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.70')
gobject_dep = dependency('gobject-2.0', version: '>= 2.70')
gio_dep = dependency('gio-2.0', version: '>= 2.70')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.6', required: false)

# libnostr - for now use include path, later can be subproject
# Note: libnostr must be built first with CMake to generate nostr-config.h
libnostr_inc = include_directories('../libnostr/include')
libnostr_generated_inc = include_directories('../libnostr/generated')

# Additional dependencies from sibling projects
libgo_inc = include_directories('../libgo/include')
gnostr_signer_inc = include_directories('../apps/gnostr-signer/src')

# External library includes (nsync for libgo context)
nsync_inc = include_directories('/opt/homebrew/include')

# Source files
nostr_glib_sources = files(
  'src/nostr-error.c',
  'src/nostr-enums.c',
  'src/nostr_event.c',
  'src/nostr_event_bus.c',
  'src/nostr_filter.c',
  'src/nostr_relay.c',
  'src/nostr_subscription.c',
  'src/nostr_simple_pool.c',
  'src/nostr_pointer.c',
  'src/nostr_relay_store.c',
  'src/crypto_utils.c',
  'src/nostr_async.c',
  'src/nostr_query_batcher.c',
  'src/nostr_tag_list.c',
  'src/nostr_subscription_registry.c',
)

# Header files
nostr_glib_headers = files(
  'include/nostr-glib.h',
  'include/nostr-error.h',
  'include/nostr-types.h',
  'include/nostr-enums.h',
  'include/nostr_event.h',
  'include/nostr_event_bus.h',
  'include/nostr_filter.h',
  'include/nostr_relay.h',
  'include/nostr_subscription.h',
  'include/nostr_simple_pool.h',
  'include/nostr_pointer.h',
  'include/nostr_relay_store.h',
  'include/crypto_utils_gobject.h',
  'include/nostr_async.h',
  'include/nostr_query_batcher.h',
  'include/nostr_tag_list.h',
  'include/nostr_subscription_registry.h',
)

# Build the library
nostr_glib_lib = library('nostr-glib-1.0',
  nostr_glib_sources,
  include_directories: ['include', libnostr_inc, libnostr_generated_inc, libgo_inc, gnostr_signer_inc, nsync_inc],
  dependencies: [glib_dep, gobject_dep, gio_dep],
  install: true)

# Declare dependency for use as subproject
nostr_glib_dep = declare_dependency(
  link_with: nostr_glib_lib,
  include_directories: include_directories('include'))

# GObject Introspection
gnome = import('gnome')

if get_option('introspection')
  nostr_glib_gir = gnome.generate_gir(nostr_glib_lib,
    sources: nostr_glib_sources + nostr_glib_headers,
    namespace: 'Nostr',
    nsversion: '1.0',
    symbol_prefix: 'nostr',
    identifier_prefix: 'Nostr',
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    install: true)
endif

# pkg-config file generation
pkg = import('pkgconfig')
pkg.generate(nostr_glib_lib,
  name: 'nostr-glib-1.0',
  description: 'GObject bindings for libnostr',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'])

# Header installation
install_headers(nostr_glib_headers, subdir: 'nostr-glib-1.0')

# Tests
if get_option('tests')
  test_nostr_event = executable('test_nostr_event',
    'tests/test_nostr_event.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_glib_lib,
    include_directories: ['include', libnostr_inc, libnostr_generated_inc, libgo_inc, gnostr_signer_inc, nsync_inc])

  test('nostr_event', test_nostr_event)
endif

# Examples
if get_option('examples')
  connect_publish = executable('connect_publish',
    'examples/connect_publish.c',
    dependencies: [glib_dep, gobject_dep],
    link_with: nostr_glib_lib,
    include_directories: ['include', libnostr_inc, libnostr_generated_inc, libgo_inc, gnostr_signer_inc, nsync_inc])
endif
