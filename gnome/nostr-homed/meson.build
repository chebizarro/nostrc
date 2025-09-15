project('nostr-homed', 'c', default_options: ['c_std=c11', 'warning_level=3', 'werror=true'])

cc = meson.get_compiler('c')

jann = dependency('jansson', required: true)
glib = dependency('glib-2.0', version: '>=2.56')
gobject = dependency('gobject-2.0', version: '>=2.56')
gio = dependency('gio-2.0', version: '>=2.56')
openssl = dependency('openssl', required: true)
secp = cc.find_library('secp256k1', required: true)
sqlite3 = dependency('sqlite3', required: true)
curl = dependency('libcurl', required: true)
fuse3 = dependency('fuse3', required: true)

inc = include_directories('include', '../..//libnostr/include', '../..//libjson/include', '../..//libgo/include')

nh_common_src = files(
  'src/common/nostr_manifest.c',
  'src/common/nostr_cache.c',
  'src/common/relay_fetch.c',
  'src/common/blossom_client.c',
  'src/common/secrets_tmpfs.c',
  'src/common/secrets_decrypt.c',
  'src/common/nip46_client_dbus.c',
  'src/common/util.c',
)

nh_common = static_library('nostr_homed_common', nh_common_src,
  include_directories: inc,
  dependencies: [jann, glib, gobject, gio, openssl, sqlite3, curl, fuse3, secp]
)

nostrfs = executable('nostrfs', 'src/fs/nostrfs.c', include_directories: inc,
  link_with: nh_common,
  dependencies: [fuse3])

nss_nostr = shared_library('nss_nostr', 'src/nss/nss_nostr.c', include_directories: inc,
  link_with: nh_common,
  soversion: '2',
  install: true, install_dir: get_option('libdir'))

pam = dependency('libpam', required: false)
if pam.found()
  pam_nostr = shared_library('pam_nostr', 'src/pam/pam_nostr.c', include_directories: inc,
    link_with: nh_common,
    install: true, install_dir: join_paths(get_option('libdir'), 'security'))
endif

nostr_homectl = executable('nostr-homectl', 'src/ctl/nostr-homectl.c', include_directories: inc,
  link_with: nh_common,
  dependencies: [glib, gio],
  install: true)

install_data('dbus/org.nostr.Homed1.xml', install_dir: join_paths(get_option('datadir'), 'dbus-1', 'interfaces'))
install_data('systemd/nostr-homectl.service', install_dir: join_paths(get_option('libdir'), 'systemd', 'system'))
install_data('systemd/nostrfs@.service', install_dir: join_paths(get_option('libdir'), 'systemd', 'system'))
install_data('systemd/nostr-homed.target', install_dir: join_paths(get_option('libdir'), 'systemd', 'system'))

# Tests
subdir('tests')
