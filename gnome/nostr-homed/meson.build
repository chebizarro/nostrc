project('nostr-homed', 'c', version: '0.2.2',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true'])

cc = meson.get_compiler('c')
python = import('python').find_installation(required: false)
inc = include_directories('include', 'src/auth', 'src/identity')

if get_option('auth_install').enabled()
  error('auth_install is unavailable: broker, PAM client, peer binding, and activation policy are incomplete; use -Dauth_core=enabled for portable tests only')
endif

if get_option('experimental_roaming').enabled()
  error('experimental_roaming is not part of the portable Meson checkpoint; use the explicit CMake experimental target until dependency discovery is replaced')
endif

if get_option('auth_core').enabled()
  jansson = dependency('jansson', required: true)
  openssl = dependency('openssl', required: true)
  nostr = dependency('nostr', required: true)
  libgo = dependency('libgo', required: true)
  auth_core = static_library('nostr_auth_core',
    'src/auth/auth_protocol.c',
    'src/auth/auth_transaction.c',
    'src/auth/auth_challenge.c',
    'src/auth/auth_vault.c',
    'src/auth/provider_local.c',
    include_directories: inc,
    dependencies: [jansson, openssl, nostr, libgo])
  auth_core_dep = declare_dependency(
    link_with: auth_core,
    include_directories: inc,
    dependencies: [jansson, openssl, nostr, libgo])
endif

if get_option('identity_core').enabled()
  sqlite3 = dependency('sqlite3', required: true)
  identity_openssl = dependency('openssl', required: true)
  identity_core = static_library('nostr_identity_core',
    'src/identity/identity_common.c',
    'src/identity/identity_config.c',
    'src/identity/identity_store.c',
    'src/identity/identity_reader.c',
    'src/identity/identity_projection.c',
    include_directories: inc,
    dependencies: [sqlite3, identity_openssl])
  identity_core_dep = declare_dependency(
    link_with: identity_core,
    include_directories: inc,
    dependencies: [sqlite3, identity_openssl])
endif

if get_option('auth_core').enabled() and get_option('identity_core').enabled()
  auth_identity = static_library('nostr_auth_identity',
    'src/auth/auth_transaction_identity.c',
    include_directories: inc,
    dependencies: [auth_core_dep, identity_core_dep])
  auth_identity_dep = declare_dependency(link_with: auth_identity,
    dependencies: [auth_core_dep, identity_core_dep])
endif

if get_option('domain_config').enabled()
  if not python.found()
    error('domain_config requires Python 3')
  endif
  domain_share = join_paths(get_option('datadir'), 'nostr-homed', 'domain')
  install_data(
    'config/domain-route.conf.sample',
    'config/smb.conf.winbind.sample',
    'packaging/domain/domain-profile.manifest.json.sample',
    'packaging/domain/ROLLBACK.md',
    'packaging/pam/nostr-winbind-policy.md',
    'docs/SAMBA_AD_LOGIN.md',
    install_dir: domain_share)
  install_data('packaging/domain/validate_domain_profile.py',
    install_mode: 'rwxr-xr-x',
    install_dir: join_paths(get_option('libexecdir'), 'nostr-homed'))
endif

if get_option('homed_tests')
  subdir('tests')
endif

pkg = import('pkgconfig')
pkg.generate(
  name: 'nostr-homed',
  description: 'Version metadata for opt-in Nostr login and experimental roaming components',
  version: meson.project_version())
