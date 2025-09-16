goa_backend = dependency('goa-backend-1.0', required: true)
glib = dependency('glib-2.0')
gobject = dependency('gobject-2.0')
gio = dependency('gio-2.0')

provider_inc = include_directories('.')

shared_library('goa-nostr-provider',
  ['goanostrprovider.c'],
  include_directories: [provider_inc],
  dependencies: [goa_backend, glib, gobject, gio],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'goa-1.0', 'backends')
)

# Install provider icon (scalable)
install_data('icons/hicolor/scalable/apps/goa-nostr.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'))
