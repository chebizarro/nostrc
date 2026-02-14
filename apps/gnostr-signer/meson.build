# gnostr-signer meson build configuration
#
# Build with: meson setup builddir && cd builddir && meson compile
#
# This provides meson build support alongside the existing CMake configuration.

project('gnostr-signer', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
  meson_version: '>= 0.59.0',
)

cc = meson.get_compiler('c')

# Application identifiers
app_id = 'org.gnostr.Signer'
app_name = 'GNostr Signer'
app_bin = 'gnostr-signer'

# i18n configuration
i18n = import('i18n')
gettext_package = 'gnostr-signer'
localedir = get_option('prefix') / get_option('localedir')

# Required dependencies
gtk4_dep = dependency('gtk4', version: '>= 4.0')
glib_dep = dependency('glib-2.0', version: '>= 2.56')
gobject_dep = dependency('gobject-2.0', version: '>= 2.56')
gio_dep = dependency('gio-2.0', version: '>= 2.56')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.0')
json_glib_dep = dependency('json-glib-1.0')

# Optional: libsecret for secure key storage on Linux
libsecret_dep = dependency('libsecret-1', required: false)

# ── nostr-gobject (pre-built by CMake) ────────────────────────────────
cmake_build_dir = meson.current_source_dir() / '..' / '..' / 'build'
nostr_gobject_shared = cc.find_library('nostr-gobject-1.0',
  dirs: [cmake_build_dir / 'nostr-gobject'],
  static: false)
nostr_gobject_inc = include_directories('../../nostr-gobject/include')
nostr_gobject_dep = declare_dependency(
  dependencies: nostr_gobject_shared,
  include_directories: nostr_gobject_inc)

# Configure compile definitions
conf_data = configuration_data()
conf_data.set_quoted('APP_ID', app_id)
conf_data.set_quoted('APP_NAME', app_name)
conf_data.set_quoted('APP_RESOURCE_PATH', '/org/gnostr/signer')

# Check for libsecret availability
if libsecret_dep.found()
  conf_data.set('GNOSTR_HAVE_LIBSECRET', 1)
  message('libsecret found - secure storage enabled')
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
  '../../nips/nip5f/include',
  '../../nips/nip77/include',
  '../../nips/nip86/include',
  '../../nips/nip94/include',
)

# Source files
app_sources = files(
  'src/main_app.c',
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
  'src/keyboard-nav.c',
  'src/memory-profile.c',
  'src/multisig_coordinator.c',
  'src/multisig_nip46.c',
  'src/multisig_store.c',
  'src/multisig_wallet.c',
  'src/policy_store.c',
  'src/profile_store.c',
  'src/qr-code.c',
  'src/qr-scanner.c',
  'src/rate-limiter.c',
  'src/relay_info.c',
  'src/relay_store.c',
  'src/secret_store.c',
  'src/secret-storage.c',
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
  'src/ui/accounts_page.c',
  'src/ui/approval_dialog.c',
  'src/ui/confirm-delete-dialog.c',
  'src/ui/events-page.c',
  'src/ui/hardware_keystore_widget.c',
  'src/ui/home_page.c',
  'src/ui/lazy-loader.c',
  'src/ui/lock-screen.c',
  'src/ui/onboarding-assistant.c',
  'src/ui/page-applications.c',
  'src/ui/page-history.c',
  'src/ui/page-permissions.c',
  'src/ui/page-sessions.c',
  'src/ui/page-settings.c',
  'src/ui/permissions_page.c',
  'src/ui/profile-dashboard.c',
  'src/ui/settings_page.c',
  'src/ui/signer-window.c',
  'src/ui/welcome-page.c',
  # Sheets
  'src/ui/sheets/sheet-account-backup.c',
  'src/ui/sheets/sheet-add-application.c',
  'src/ui/sheets/sheet-backup.c',
  'src/ui/sheets/sheet-change-password.c',
  'src/ui/sheets/sheet-create-account.c',
  'src/ui/sheets/sheet-create-bunker.c',
  'src/ui/sheets/sheet-create-multisig.c',
  'src/ui/sheets/sheet-create-profile.c',
  'src/ui/sheets/sheet-delegation.c',
  'src/ui/sheets/sheet-event-details.c',
  'src/ui/sheets/sheet-hw-wallet.c',
  'src/ui/sheets/sheet-import-key.c',
  'src/ui/sheets/sheet-import-profile.c',
  'src/ui/sheets/sheet-key-rotation.c',
  'src/ui/sheets/sheet-multisig-signing.c',
  'src/ui/sheets/sheet-orbot-setup.c',
  'src/ui/sheets/sheet-profile-editor.c',
  'src/ui/sheets/sheet-qr-display.c',
  'src/ui/sheets/sheet-qr-scanner.c',
  'src/ui/sheets/sheet-relay-config.c',
  'src/ui/sheets/sheet-select-account.c',
  'src/ui/sheets/sheet-sign-message.c',
  'src/ui/sheets/sheet-social-recovery.c',
  'src/ui/sheets/sheet-user-list.c',
  # Widgets
  'src/ui/widgets/app-row.c',
  'src/ui/widgets/gn-secure-entry.c',
  'src/ui/widgets/identity-row.c',
  'src/ui/widgets/relay-row.c',
)

# GResource compilation
gnome = import('gnome')
app_resources = gnome.compile_resources(
  'gnostr-signer-resources',
  'data/resources.gresource.xml',
  source_dir: 'data',
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
    root_inc,
    libnostr_inc,
    libgo_inc,
    nips_inc,
  ],
  dependencies: app_deps,
  c_args: [
    '-DAPP_ID="@0@"'.format(app_id),
    '-DAPP_NAME="@0@"'.format(app_name),
    '-DAPP_RESOURCE_PATH="/org/gnostr/signer"',
    '-DGETTEXT_PACKAGE="@0@"'.format(gettext_package),
    '-DLOCALEDIR="@0@"'.format(localedir),
    libsecret_dep.found() ? '-DGNOSTR_HAVE_LIBSECRET=1' : [],
  ].flatten(),
  install: true,
)

# Install GSettings schema
install_data('data/org.gnostr.Signer.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

# Install D-Bus service file
install_data('data/org.nostr.Signer.service',
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

# Install desktop file
install_data('packaging/appimage/gnostr-signer.desktop',
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
