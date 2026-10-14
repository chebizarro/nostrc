# grotto meson build configuration
#
# Build with: meson setup builddir && cd builddir && meson compile
#
# This provides meson build support alongside the existing CMake configuration.

project('grotto', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ],
  meson_version: '>= 0.59.0',
)

cc = meson.get_compiler('c')

# Application identifiers
app_id = 'org.nostr.Grotto'
app_name = 'Grotto'
app_bin = 'grotto'

# i18n configuration
i18n = import('i18n')
gettext_package = 'grotto'
localedir = get_option('prefix') / get_option('localedir')

# Required dependencies
gtk4_dep = dependency('gtk4', version: '>= 4.14')
glib_dep = dependency('glib-2.0', version: '>= 2.80')
gobject_dep = dependency('gobject-2.0', version: '>= 2.80')
gio_dep = dependency('gio-2.0', version: '>= 2.80')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.5')
json_glib_dep = dependency('json-glib-1.0')
native_camera_required = host_machine.system() == 'linux'
qrencode_dep = dependency('libqrencode', required: native_camera_required)
zbar_dep = dependency('zbar', required: native_camera_required)
gst_dep = dependency('gstreamer-1.0', required: native_camera_required)
gst_video_dep = dependency('gstreamer-video-1.0', required: native_camera_required)
gst_app_dep = dependency('gstreamer-app-1.0', required: native_camera_required)
have_camera = gst_dep.found() and gst_video_dep.found() and gst_app_dep.found()

# Optional: libsecret for secure key storage on Linux
libsecret_dep = dependency('libsecret-1', required: false)

# ── nostr-gobject (pre-built by CMake) ────────────────────────────────
cmake_build_dir = get_option('cmake_build_dir')
if cmake_build_dir == ''
  cmake_build_dir = meson.current_source_dir() / '..' / '..' / 'build'
endif
nostr_gobject_shared = cc.find_library('nostr-gobject-1.0',
  dirs: [cmake_build_dir / 'nostr-gobject'])
nostr_gobject_inc = include_directories('../../nostr-gobject/include')
# BunkerService now submits to the shared daemon gate (no GUI crypto path).
nip55l_glib_lib = cc.find_library('nostr_nip55l_glib', dirs: [cmake_build_dir / 'nips/nip55l'])
nip55l_core_lib = cc.find_library('nostr_nip55l_core', dirs: [cmake_build_dir / 'nips/nip55l'])
nostr_gobject_dep = declare_dependency(
  dependencies: nostr_gobject_shared,
  include_directories: nostr_gobject_inc)

# Dependencies of the prebuilt archives, matching the CMake GUI linkage.
# Meson owns the GUI/templates; the daemon and protocol libraries remain CMake-owned.
protocol_deps = []
foreach component : [
  ['nostr_nip46_core', 'nips/nip46'], ['nip04', 'nips/nip04'], ['nip06', 'nips/nip06'],
  ['nip19', 'nips/nip19'], ['nip11', 'nips/nip11'], ['nostr_nip44_core', 'nips/nip44'],
  ['nostr_nip49_core', 'nips/nip49'], ['nostr_nip49_glib', 'nips/nip49'],
  ['nostr_nip5f_core', 'nips/nip5f'], ['gnostr-secret', 'gnome/seahorse'],
  ['nostr', 'libnostr'], ['nostrgo', 'libgo'], ['nostr-json', 'libjson'],
  ['nostrdb_storage', 'components/nostrdb'], ['nostrdb_vendored', 'components/nostrdb'],
  ['nostrdb', 'libnostr']]
  protocol_deps += cc.find_library(component[0], dirs: [cmake_build_dir / component[1]])
endforeach
foreach library : ['openssl', 'libsodium', 'libsecp256k1', 'libwebsockets', 'jansson', 'libcurl']
  protocol_deps += dependency(library)
endforeach
dependency_library_dirs = []
dependency_include_dirs = []
foreach prefix : get_option('dependency_prefixes')
  dependency_library_dirs += prefix / 'lib'
  dependency_include_dirs += include_directories(prefix / 'include')
endforeach
system_libdir = glib_dep.get_variable(pkgconfig: 'libdir')
protocol_deps += cc.find_library('nsync', dirs: dependency_library_dirs + [system_libdir, cmake_build_dir / 'third_party/nsync'])
flatcc_dep = cc.find_library('flatccrt', required: false,
  dirs: dependency_library_dirs + [system_libdir, cmake_build_dir / 'components/nostrdb/flatcc/lib'])
if flatcc_dep.found()
  protocol_deps += flatcc_dep
endif
protocol_deps += dependency('threads')

# Configure compile definitions
conf_data = configuration_data()
conf_data.set_quoted('APP_ID', app_id)
conf_data.set_quoted('APP_NAME', app_name)
conf_data.set_quoted('APP_RESOURCE_PATH', '/org/gnostr/signer')

# Check for libsecret availability
if libsecret_dep.found()
  message('libsecret linked; daemon owns key writes')
else
  message('libsecret not found - using fallback storage')
endif

# macOS Keychain support (detected by host system)
if host_machine.system() == 'darwin'
  conf_data.set('GNOSTR_HAVE_KEYCHAIN', 1)
  security_dep = dependency('appleframeworks', modules: ['Security', 'CoreFoundation'])
  message('macOS detected - Keychain support enabled')
else
  security_dep = []
endif

# Include directories for NIP libraries
root_inc = include_directories('../../')
libnostr_inc = include_directories('../../libnostr/include')
libgo_inc = include_directories('../../libgo/include')
nips_inc = include_directories(
  '../../nips/nip01/include',
  '../../nips/nip02/include',
  '../../nips/nip04/include',
  '../../nips/nip05/include',
  '../../nips/nip06/include',
  '../../nips/nip10/include',
  '../../nips/nip11/include',
  '../../nips/nip17/include',
  '../../nips/nip19/include',
  '../../nips/nip29/include',
  '../../nips/nip31/include',
  '../../nips/nip34/include',
  '../../nips/nip42/include',
  '../../nips/nip44/include',
  '../../nips/nip45/include',
  '../../nips/nip46/include',
  '../../nips/nip47/include',
  '../../nips/nip49/include',
  '../../nips/nip50/include',
  '../../nips/nip51/include',
  '../../nips/nip52/include',
  '../../nips/nip53/include',
  '../../nips/nip54/include',
  '../../nips/nip55l/include',
  '../../nips/nip55l/src/glib',
  '../../nips/nip5f/include',
  '../../nips/nip77/include',
  '../../nips/nip86/include',
  '../../nips/nip94/include',
  '../../gnome/common',
)

# Source files
app_sources = files(
  'src/main_app.c',
  'src/legacy_dirs.c',
  'src/home-model.c',
  'src/gn-signer-error.c',
  'src/approval-controller.c',
  'src/background.c',
  '../../gnome/common/gn-session-bus-probe.c',
  '../../gnome/common/gn-status-notifier.c',
  'src/accounts_store.c',
  'src/backup-recovery.c',
  'src/bunker_service.c',
  'src/cache-manager.c',
  'src/client_session.c',
  'src/delegation.c',
  'src/event_history.c',
  'src/hsm_provider.c',
  'src/hsm_provider_mock.c',
  'src/hsm_provider_pkcs11.c',
  'src/hsm_provider_tpm.c',
  'src/hw_keystore_manager.c',
  'src/hw_wallet_ledger.c',
  'src/hw_wallet_provider.c',
  'src/hw_wallet_trezor.c',
  'src/i18n.c',
  'src/key_provider.c',
  'src/key_provider_ed25519.c',
  'src/key_provider_secp256k1.c',
  'src/key_rotation.c',
  'src/key-migration-event.c',
  'src/keyboard-nav.c',
  'src/memory-profile.c',
  'src/multisig_coordinator.c',
  'src/multisig_nip46.c',
  'src/multisig_store.c',
  'src/multisig_wallet.c',
  'src/signer_grants.c',
  'src/profile_store.c',
  'src/pairing_uri.c',
  'src/qr-code.c',
  'src/qr-scanner.c',
  'src/rate-limiter.c',
  'src/relay_info.c',
  'src/relay_store.c',
  'src/secret_store.c',
  'src/secure-delete.c',
  'src/secure-mem.c',
  'src/secure-memory.c',
  'src/session-manager.c',
  'src/settings_manager.c',
  'src/social-recovery.c',
  'src/startup-timing.c',
  'src/user_list_store.c',
)

# UI source files
app_ui_sources = files(
  'src/ui/approval_dialog.c',
  'src/ui/lock-screen.c',
  'src/ui/onboarding-assistant.c',
  'src/ui/page-home.c',
  'src/ui/page-connect.c',
  'src/ui/page-client-detail.c',
  'src/ui/page-identity-detail.c',
  'src/secret-clipboard.c',
  'src/ui/page-history.c',
  'src/ui/page-settings.c',
  'src/ui/signer-window.c',
  'src/ui/pairing-flow.c',
  'src/ui/sheets/sheet-create-bunker.c',
  'src/ui/sheets/sheet-qr-display.c',
  'src/ui/sheets/sheet-qr-scanner.c',
  'src/ui/widgets/gn-secure-entry.c',
)

# GResource compilation
gnome = import('gnome')
blueprint = find_program('blueprint-compiler')
if run_command(blueprint, '--version', check: true).stdout().strip() != '0.22.2'
  error('Grotto Blueprint compiler is pinned to 0.22.2')
endif
blueprints = custom_target('blueprints',
  input: files(
    'data/ui/signer-window.blp', 'data/ui/page-home.blp', 'data/ui/page-connect.blp',
    'data/ui/page-client-detail.blp', 'data/ui/page-history.blp', 'data/ui/page-settings.blp',
    'data/ui/page-identity-detail.blp', 'data/ui/approval-dialog.blp',
    'data/ui/lock-screen.blp', 'data/ui/onboarding-assistant.blp', 'data/ui/shortcuts-window.blp',
    'data/ui/sheets/sheet-create-bunker.blp', 'data/ui/sheets/sheet-qr-display.blp',
    'data/ui/sheets/sheet-qr-scanner.blp', 'data/ui/widgets/gn-secure-entry.blp'),
  output: 'data',
  command: [blueprint, 'batch-compile', '@OUTPUT@', meson.current_source_dir() / 'data', '@INPUT@'])
app_resources = gnome.compile_resources(
  'grotto-resources',
  'data/resources.gresource.xml',
  source_dir: [meson.current_build_dir() / 'data', 'data'],
  dependencies: blueprints,
  c_name: 'gnostr_signer',
)

# Build dependencies list
app_deps = [
  gtk4_dep,
  glib_dep,
  gobject_dep,
  gio_dep,
  adwaita_dep,
  json_glib_dep,
  nostr_gobject_dep,
]
foreach qr_dep : [qrencode_dep, zbar_dep]
  if qr_dep.found()
    app_deps += qr_dep
  endif
endforeach
if have_camera
  app_deps += [gst_dep, gst_video_dep, gst_app_dep]
endif

if libsecret_dep.found()
  app_deps += libsecret_dep
endif

if host_machine.system() == 'darwin'
  app_deps += security_dep
endif

# Main executable
executable(app_bin,
  app_sources,
  app_ui_sources,
  app_resources,
  include_directories: [
    include_directories(cmake_build_dir / 'libnostr/generated'),
    root_inc,
    libnostr_inc,
    libgo_inc,
    nips_inc,
    nostr_gobject_inc,
  ] + dependency_include_dirs,
  dependencies: app_deps + [nip55l_glib_lib, nip55l_core_lib] + protocol_deps,
  c_args: [
    '-DAPP_ID="@0@"'.format(app_id),
    '-DAPP_NAME="@0@"'.format(app_name),
    '-DAPP_RESOURCE_PATH="/org/gnostr/signer"',
    '-DGETTEXT_PACKAGE="@0@"'.format(gettext_package),
    '-DLOCALEDIR="@0@"'.format(localedir),
    '-DGROTTO_EXECUTABLE="@0@/grotto"'.format(get_option('prefix') / get_option('bindir')),
    '-DGROTTO_AUTOSTART_TEMPLATE="@0@/org.nostr.Grotto-autostart.desktop"'.format(get_option('prefix') / get_option('datadir') / 'grotto'),
    '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14',
    '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14',
    '-DGTK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14',
    '-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5',
    '-DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5',
    qrencode_dep.found() ? '-DHAVE_QRENCODE=1' : [],
    zbar_dep.found() ? '-DHAVE_ZBAR=1' : [],
    have_camera ? '-DHAVE_GSTREAMER=1' : [],
  ],
  install: true,
)

# Install GSettings schema
install_data('data/org.nostr.Grotto.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

# CMake builds the daemon; Meson currently builds only the GUI. Keep the
# canonical management XML available to clients in either installation.
install_data('data/org.nostr.Grotto.Control1.xml',
  install_dir: get_option('datadir') / 'dbus-1' / 'interfaces')

# Install D-Bus activation file, configured with the absolute daemon path
dbus_service_conf = configuration_data()
dbus_service_conf.set('CMAKE_INSTALL_FULL_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(
  input: 'data/org.nostr.Signer.service.in',
  output: 'org.nostr.Signer.service',
  configuration: dbus_service_conf,
  format: 'cmake@',
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

configure_file(
  input: 'data/org.nostr.Grotto.service.in',
  output: 'org.nostr.Grotto.service',
  configuration: dbus_service_conf,
  format: 'cmake@',
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
autostart_conf = configuration_data()
autostart_conf.set('GROTTO_BINDIR', get_option('prefix') / get_option('bindir'))
configure_file(input: 'data/org.nostr.Grotto-autostart.desktop.in',
  output: 'org.nostr.Grotto-autostart.desktop', configuration: autostart_conf,
  install_dir: get_option('datadir') / 'grotto')
install_data('data/org.nostr.Grotto.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')
install_data('data/icons/hicolor/scalable/apps/org.nostr.Grotto-symbolic.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
foreach size : ['16', '24', '32', '48', '64', '128', '256', '512']
  install_data('data/icons/hicolor/' + size + 'x' + size + '/apps/org.nostr.Grotto.png',
    install_dir: get_option('datadir') / 'icons/hicolor' / (size + 'x' + size) / 'apps')
endforeach

# Install desktop file
install_data('data/org.nostr.Grotto.desktop',
  install_dir: get_option('datadir') / 'applications',
)

# Process translations
subdir('po')

# Summary
summary({
  'libsecret': libsecret_dep.found() ? 'yes' : 'no',
  'Keychain': host_machine.system() == 'darwin' ? 'yes' : 'no',
}, section: 'Secure Storage')

summary({
  'Translations': true,
  'Languages': 'ja, es, pt_BR, id, fa',
}, section: 'Internationalization')
