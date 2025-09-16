install_data('org.gnome.OnlineAccounts.service.in',
  install_dir: join_paths(get_option('prefix'), 'share', 'dbus-1', 'services'),
  rename: 'org.gnome.OnlineAccounts.service')

install_data('nostr.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'))

# Optional custom session entry to seed XDG_DATA_DIRS with ~/.local/share
install_data('sessions/gnome-nostr.desktop',
  install_dir: join_paths(get_option('datadir'), 'xsessions'))
