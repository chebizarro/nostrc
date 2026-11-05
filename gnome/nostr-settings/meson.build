# SPDX-License-Identifier: MIT
project('nostr-settings', 'c', version: '0.1.0', meson_version: '>=0.59',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=true'])
cc = meson.get_compiler('c')
deps = [dependency('glib-2.0', version: '>=2.76'), dependency('gio-2.0'),
  dependency('gio-unix-2.0'), dependency('gobject-2.0'), dependency('json-glib-1.0'),
  dependency('libsoup-3.0'), dependency('gtk4', version: '>=4.14'),
  dependency('libadwaita-1', version: '>=1.5'), dependency('grotto-panels-0', version: '>=0.1.0'),
  dependency('nostr-publish'), dependency('nostr'), cc.find_library('nip19')]
add_project_arguments('-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_76',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_80',
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14', '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14',
  '-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5', '-DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5',
  '-DNSS_VERSION="' + meson.project_version() + '"',
  '-Wno-unused-parameter', '-Wno-missing-field-initializers', language: 'c')
core = static_library('nostr-settings-core',
  'src/core/nss-config.c', 'src/core/nss-lists.c', 'src/core/nss-nip11.c',
  'src/core/nss-net.c', 'src/core/nss-systemd.c', 'src/core/nss-relay-stats.c',
  'src/core/nss-wallet.c', 'src/core/nss-identity.c',
  include_directories: include_directories('src/core'), dependencies: deps)
ui = static_library('nostr-settings-ui',
  '../../apps/grotto/src/grotto-panels-control1-service.c', 'src/ui/nss-grotto.c',
  'src/ui/nss-window.c', 'src/ui/nss-widgets.c', 'src/ui/nss-page-identity.c',
  'src/ui/nss-page-relays.c', 'src/ui/nss-page-notifications.c', 'src/ui/nss-page-wallet.c',
  'src/ui/nss-page-media.c', 'src/ui/nss-page-files.c',
  include_directories: include_directories('src/core', 'src/ui', '../../apps/grotto/src'), dependencies: deps)
executable('nostr-settings', 'src/ui/main.c', link_with: [ui, core],
  include_directories: include_directories('src/ui'), dependencies: deps, install: true)
conf = configuration_data()
conf.set('NSS_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'data/org.nostr.Settings.desktop.in', output: 'org.nostr.Settings.desktop',
  configuration: conf, install_dir: get_option('datadir') / 'applications')
configure_file(input: 'data/org.nostr.Settings.service.in', output: 'org.nostr.Settings.service',
  configuration: conf, install_dir: get_option('datadir') / 'dbus-1/services')
install_data('data/org.nostr.Settings.gschema.xml', install_dir: get_option('datadir') / 'glib-2.0/schemas')
install_data('README.md', install_dir: get_option('datadir') / 'doc/nostr-settings')
# A real-daemon mount is selected only with an explicitly supplied test-built
# daemon. Missing bus/display/role support fails the selected test, never skips.
daemon = get_option('test_daemon')
if daemon != ''
  mount = executable('test-nostr-settings-panel-mount', 'tests/test-panel-mount.c',
    '../../tests/common/nostrc-test-bus.c', link_with: [ui, core], dependencies: deps,
    include_directories: include_directories('../..', 'src/ui'),
    c_args: ['-DW6_CONTROL_XML="' + meson.current_source_dir() / '../../apps/grotto/data/org.nostr.Grotto.Control1.xml' + '"'])
  test('nostr-settings/panel-mount', find_program('xvfb-run'), args: ['-a', mount, daemon],
    env: ['GSETTINGS_BACKEND=memory', 'GTK_A11Y=test'], timeout: 120)
endif
