executable('goa_shims', 'goa_shims.c',
  include_directories: include_directories('.'),
  dependencies: [glib, gio],
  install: true,
  install_dir: get_option('bindir')
)

install_subdir('eds_sources', install_dir: join_paths(get_option('datadir'), 'nostr-goa-overlay', 'eds_sources'))
install_subdir('systemd_user', install_dir: join_paths(get_option('datadir'), 'nostr-goa-overlay', 'systemd_user'))
