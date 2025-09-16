goa_backend = dependency('goa-backend-1.0', required: false)

if not goa_backend.found()
  error('goa-backend-1.0 not found. Build vendor GOA under vendor/ and set PKG_CONFIG_PATH accordingly.')
endif

provider_inc = include_directories('.')

shared_library('goa-nostr-provider',
  ['goanostrprovider.c'],
  include_directories: [provider_inc],
  dependencies: [goa_backend, glib, gobject, gio],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'goa-1.0', 'backends')
)
