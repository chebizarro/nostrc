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

# ── libgo (pre-built by CMake) ─────────────────────────────────────────
# relay_fetch.c uses GoChannel + go_select_timeout for async event waiting.
_cmake_build = meson.project_source_root() / '../../build'
libgo_static = cc.find_library('libgo',
  dirs: [_cmake_build / 'libgo'],
  static: true,
  required: false)
go_fiber_static = cc.find_library('go_fiber',
  dirs: [_cmake_build / 'libgo'],
  static: true,
  required: false)
nsync_dep = cc.find_library('nsync', required: true)

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

nh_common_deps = [jann, glib, gobject, gio, openssl, sqlite3, curl, fuse3, secp, nsync_dep]
if libgo_static.found()
  nh_common_deps += libgo_static
endif
if go_fiber_static.found()
  nh_common_deps += go_fiber_static
endif

nh_common = static_library('nostr_homed_common', nh_common_src,
  include_directories: inc,
  dependencies: nh_common_deps
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
    dependencies: [pam, glib, gio],
    install: true, install_dir: join_paths(get_option('libdir'), 'security'))
else
  message('libpam not found; skipping pam_nostr module')
endif

nostr_homectl = executable('nostr-homectl', 'src/ctl/nostr-homectl.c', include_directories: inc,
  link_with: nh_common,
  dependencies: [glib, gio],
  install: true)

install_data('config/nss_nostr.conf.sample', install_dir: get_option('sysconfdir'))
install_data('dbus/org.nostr.Homed1.xml', install_dir: join_paths(get_option('datadir'), 'dbus-1', 'interfaces'))
# User services — see docs/SYSTEMD_TOPOLOGY.md
# Use pkg-config systemduserunitdir; fall back to lib/systemd/user
systemd_dep = dependency('systemd', required: false)
if systemd_dep.found()
  systemd_user_unit_dir = systemd_dep.get_variable(pkgconfig: 'systemduserunitdir')
else
  systemd_user_unit_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'user')
  message('systemd not found via pkg-config; using fallback: ' + systemd_user_unit_dir)
endif
install_data('systemd/user/nostr-homectl.service', install_dir: systemd_user_unit_dir)
install_data('systemd/user/nostrfs@.service', install_dir: systemd_user_unit_dir)

# Tests
subdir('tests')
