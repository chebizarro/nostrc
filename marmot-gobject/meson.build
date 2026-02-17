project('marmot-gobject', 'c',
  version: '1.0.0',
  license: 'MIT',
  meson_version: '>= 0.59.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

# ── GLib / GObject / GIO ─────────────────────────────────────────────
glib_dep    = dependency('glib-2.0',    version: '>= 2.70')
gobject_dep = dependency('gobject-2.0', version: '>= 2.70')
gio_dep     = dependency('gio-2.0',     version: '>= 2.70')

# ── libmarmot ────────────────────────────────────────────────────────
# When built as part of the nostrc tree, the parent project provides
# marmot_dep via declare_dependency(). Otherwise, fall back to pkg-config.
cmake_build_dir = meson.current_source_dir() / '..' / 'build'

if not is_variable('marmot_dep')
  # Try pkg-config first
  marmot_dep = dependency('marmot', required: false)

  if not marmot_dep.found()
    # Fall back to pre-built static from CMake build dir
    libmarmot_static = cc.find_library('marmot',
      dirs: [cmake_build_dir / 'libmarmot'],
      static: true,
      required: true)
    libmarmot_inc = include_directories(
      '../libmarmot/include',
    )
    marmot_dep = declare_dependency(
      dependencies: libmarmot_static,
      include_directories: libmarmot_inc)
  endif
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
  include_directories: [marmot_gobject_inc],
  dependencies: marmot_gobject_deps,
  install: true,
  version: meson.project_version(),
  soversion: version_parts[0],
)

# ── Declare dependency for consumers / subprojects ───────────────────
marmot_gobject_dep = declare_dependency(
  link_with: marmot_gobject_lib,
  include_directories: marmot_gobject_pub_inc,
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
    include_directories: marmot_gobject_pub_inc,
  )
  test('marmot-gobject:all', test_marmot_gobject, env: test_env, timeout: 120)
endif
