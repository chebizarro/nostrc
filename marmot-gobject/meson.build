project('marmot-gobject', 'c',
  version: '1.5.1',
  license: 'MIT',
  meson_version: '>= 0.59.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
fs = import('fs')
export_conf = configuration_data()
export_conf.set('API', 'MARMOT_GOBJECT_API')
export_conf.set('STATIC', get_option('default_library') == 'static' ? 1 : 0)
export_conf.set('STATIC_DEFINE', 'MARMOT_GOBJECT_STATIC_DEFINE')
export_conf.set('EXPORTS', 'marmot_gobject_EXPORTS')
configure_file(input: '../cmake/meson-export.h.in',
  output: 'marmot-gobject-export.h', configuration: export_conf,
  install_dir: get_option('includedir') / 'marmot-gobject-1.0')

# ── GLib / GObject / GIO ─────────────────────────────────────────────
glib_dep    = dependency('glib-2.0',    version: '>= 2.70')
gobject_dep = dependency('gobject-2.0', version: '>= 2.70')
gio_dep     = dependency('gio-2.0',     version: '>= 2.70')

# ── libmarmot ────────────────────────────────────────────────────────
# When built as part of the nostrc tree, the parent provides marmot_dep.
# Standalone Meson builds bind to the selected CMake tree, never a system
# libmarmot with the same name. Its build-tree pkg-config export contains
# exact target artifact paths and the CMake target's static link closure.
cmake_build_dir = get_option('cmake_build_dir')
if cmake_build_dir == ''
  cmake_build_dir = meson.current_source_dir() / '..' / 'build'
endif

if not is_variable('marmot_dep')
  marmot_static = cmake_build_dir / 'libmarmot/libmarmot.a'
  marmot_shared = cmake_build_dir / 'libmarmot' / (
    host_machine.system() == 'darwin' ? 'libmarmot.dylib' : 'libmarmot.so')
  if not fs.is_file(marmot_static) and not fs.is_file(marmot_shared)
    error('Required CMake libmarmot artifact missing in ' + (cmake_build_dir / 'libmarmot'))
  endif

  marmot_link_export = cmake_build_dir / 'libmarmot/marmot-build-tree.pc'
  if not fs.is_file(marmot_link_export)
    error('Required CMake libmarmot link export missing: ' + marmot_link_export)
  endif
  pkg_config = find_program('pkg-config')
  marmot_link_args = run_command(pkg_config, '--libs', '--static',
    marmot_link_export, check: true).stdout().strip().split()
  marmot_dep = declare_dependency(
    link_args: marmot_link_args,
    include_directories: include_directories(
      '../libmarmot/include',
      cmake_build_dir / 'libmarmot/include'))
endif

# ── Version header ───────────────────────────────────────────────────
version_parts = meson.project_version().split('.')
version_conf = configuration_data()
version_conf.set('MARMOT_GOBJECT_MAJOR_VERSION', version_parts[0])
version_conf.set('MARMOT_GOBJECT_MINOR_VERSION', version_parts[1])
version_conf.set('MARMOT_GOBJECT_MICRO_VERSION', version_parts[2])
version_conf.set('MARMOT_GOBJECT_VERSION', meson.project_version())

marmot_gobject_version_h = configure_file(
  input: 'include/marmot-gobject-1.0/marmot-gobject-version.h.in',
  output: 'marmot-gobject-version.h',
  configuration: version_conf,
  install: true,
  install_dir: get_option('includedir') / 'marmot-gobject-1.0',
)

# ── GNOME module (GIR / VAPI) ───────────────────────────────────────
gnome = import('gnome')

# ── Source files ─────────────────────────────────────────────────────
marmot_gobject_sources = files(
  'src/marmot-gobject-enums.c',
  'src/marmot-gobject-group.c',
  'src/marmot-gobject-message.c',
  'src/marmot-gobject-welcome.c',
  'src/marmot-gobject-storage.c',
  'src/marmot-gobject-key-package.c',
  'src/marmot-gobject-boxed.c',
  'src/marmot-gobject-client.c',
)

marmot_gobject_headers = files(
  'include/marmot-gobject-1.0/marmot-gobject.h',
  'include/marmot-gobject-1.0/marmot-gobject-enums.h',
  'include/marmot-gobject-1.0/marmot-gobject-client.h',
  'include/marmot-gobject-1.0/marmot-gobject-group.h',
  'include/marmot-gobject-1.0/marmot-gobject-message.h',
  'include/marmot-gobject-1.0/marmot-gobject-welcome.h',
  'include/marmot-gobject-1.0/marmot-gobject-storage.h',
  'include/marmot-gobject-1.0/marmot-gobject-key-package.h',
  'include/marmot-gobject-1.0/marmot-gobject-boxed.h',
)

# ── Include directories ──────────────────────────────────────────────
# Internal: library sources use #include "marmot-gobject-client.h" (flat)
marmot_gobject_inc = include_directories('include/marmot-gobject-1.0')

# External: consumers use #include <marmot-gobject-1.0/marmot-gobject.h>
marmot_gobject_pub_inc = include_directories('include')

# ── Dependencies list ────────────────────────────────────────────────
marmot_gobject_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  marmot_dep,
]

# ── Build the library ────────────────────────────────────────────────
marmot_gobject_lib = library('marmot-gobject-1.0',
  marmot_gobject_sources,
  include_directories: [marmot_gobject_inc, marmot_gobject_pub_inc],
  dependencies: marmot_gobject_deps,
  install: true,
  version: meson.project_version(),
  gnu_symbol_visibility: 'hidden',
  c_args: ['-Dmarmot_gobject_EXPORTS'],
  link_args: cc.get_supported_link_arguments('-Wl,--exclude-libs,ALL'),
  soversion: version_parts[0],
)

# ── Declare dependency for consumers / subprojects ───────────────────
marmot_gobject_dep = declare_dependency(
  link_with: marmot_gobject_lib,
  include_directories: [marmot_gobject_pub_inc, include_directories('.')],
  dependencies: [glib_dep, gobject_dep, gio_dep, marmot_dep],
)

# ── GObject Introspection ────────────────────────────────────────────
if get_option('introspection')
  marmot_gobject_gir = gnome.generate_gir(marmot_gobject_lib,
    sources: marmot_gobject_sources + marmot_gobject_headers,
    namespace: 'Marmot',
    nsversion: '1.0',
    symbol_prefix: 'marmot_gobject',
    identifier_prefix: 'MarmotGobject',
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    header: 'marmot-gobject-1.0/marmot-gobject.h',
    install: true,
  )

  # ── VAPI generation (Vala bindings) ──────────────────────────────
  if get_option('vapi')
    gnome.generate_vapi('marmot-gobject-1.0',
      sources: marmot_gobject_gir[0],
      packages: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
      install: true,
    )
  endif
endif

# ── pkg-config ───────────────────────────────────────────────────────
pkg = import('pkgconfig')
pkg.generate(marmot_gobject_lib,
  name: 'marmot-gobject-1.0',
  description: 'GObject wrapper for libmarmot — Marmot protocol (MLS + Nostr)',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
  requires_private: ['marmot'],
)

# ── Header installation ──────────────────────────────────────────────
install_headers(marmot_gobject_headers, subdir: 'marmot-gobject-1.0')

# ── Tests ─────────────────────────────────────────────────────────────
if get_option('tests')
  test_env = environment()
  test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
  test_env.set('G_SLICE', 'always-malloc')

  test_marmot_gobject = executable('test_marmot_gobject',
    'tests/test_marmot_gobject.c',
    dependencies: [glib_dep, gobject_dep, gio_dep, marmot_dep],
    link_with: marmot_gobject_lib,
    include_directories: [marmot_gobject_pub_inc, include_directories('.'),
      include_directories('../libnostr/include', cmake_build_dir / 'libnostr/include'),
      include_directories('../libgo/include', cmake_build_dir / 'libgo/include')],
  )
  test('marmot-gobject:all', test_marmot_gobject, env: test_env, timeout: 120)
endif
