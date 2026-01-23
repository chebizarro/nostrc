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
nip55l_inc = include_directories('../../nips/nip55l/include')
nip46_inc = include_directories('../../nips/nip46/include')
nip19_inc = include_directories('../../nips/nip19/include')

# Source files
app_sources = files(
  'src/main_app.c',
  'src/accounts_store.c',
  'src/bunker_service.c',
  'src/cache-manager.c',
  'src/i18n.c',
  'src/memory-profile.c',
  'src/policy_store.c',
  'src/profile_store.c',
  'src/relay_info.c',
  'src/relay_store.c',
  'src/secret_store.c',
  'src/secret-storage.c',
  'src/settings_manager.c',
  'src/user_list_store.c',
)

# UI source files
app_ui_sources = []
ui_src_dir = 'src/ui'
if run_command('test', '-d', ui_src_dir, check: false).returncode() == 0
  # Add UI sources if directory exists
  app_ui_sources = files(
    'src/ui/account_settings.c',
    'src/ui/about_dialog.c',
    'src/ui/bunker_connect_dialog.c',
    'src/ui/connection_manager_page.c',
    'src/ui/lazy-loader.c',
    'src/ui/main_window.c',
    'src/ui/relay_page.c',
    'src/ui/theme.c',
  )
endif

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
    nip55l_inc,
    nip46_inc,
    nip19_inc,
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
  'Languages': 'en, ja, es, pt_BR, id, fa',
}, section: 'Internationalization')
