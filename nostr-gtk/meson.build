project('nostr-gtk', 'c',
  version: '1.2.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.2.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized'
  ])

cc = meson.get_compiler('c')

# ── Dependencies ─────────────────────────────────────────────────────
glib_dep    = dependency('glib-2.0',    version: '>= 2.70')
gobject_dep = dependency('gobject-2.0', version: '>= 2.70')
gio_dep     = dependency('gio-2.0',     version: '>= 2.70')
gtk4_dep    = dependency('gtk4',        version: '>= 4.6')
adw_dep     = dependency('libadwaita-1', version: '>= 1.2')
json_glib_dep = dependency('json-glib-1.0', required: true)
soup3_dep   = dependency('libsoup-3.0', required: false)

# nostr-gobject-1.0 (sibling library, pre-built)
cmake_build_dir = meson.current_source_dir() / '..' / 'build'

nostr_gobject_wrapper = cc.find_library('nostr_gobject_shared',
  dirs: [cmake_build_dir / 'nostr-gobject'],
  required: false)
nostr_gobject_lib = nostr_gobject_wrapper
if not (get_option('introspection') and nostr_gobject_wrapper.found())
  nostr_gobject_lib = cc.find_library('nostr-gobject-1.0',
    dirs: [cmake_build_dir / 'nostr-gobject'],
    required: true)
endif
nostr_gobject_inc = include_directories('../nostr-gobject/include')
nostr_gobject_dep = declare_dependency(
  dependencies: nostr_gobject_lib,
  include_directories: nostr_gobject_inc)

# ── Version header ───────────────────────────────────────────────────
version_parts = meson.project_version().split('.')
version_conf = configuration_data()
version_conf.set('NOSTR_GTK_MAJOR_VERSION', version_parts[0])
version_conf.set('NOSTR_GTK_MINOR_VERSION', version_parts[1])
version_conf.set('NOSTR_GTK_MICRO_VERSION', version_parts[2])
version_conf.set('NOSTR_GTK_VERSION', meson.project_version())

nostr_gtk_version_h = configure_file(
  input: 'include/nostr-gtk-1.0/nostr-gtk-version.h.in',
  output: 'nostr-gtk-version.h',
  configuration: version_conf,
  install: true,
  install_dir: get_option('includedir') / 'nostr-gtk-1.0')

# ── GNOME module (GResource, Blueprint, GIR) ────────────────────────
gnome = import('gnome')

# ── Blueprint support ─────────────────────────────────────────────────────────
blueprint_compiler = find_program('blueprint-compiler', required: false)
nostr_gtk_blueprint_deps = []
if blueprint_compiler.found()
  # Compile all blueprint files into the source data/ tree so GResource can find them
  _blp_sources = [
    ['data/ui/nostr-note-card-row.blp', 'data/ui/nostr-note-card-row.ui'],
    ['data/ui/gnostr-composer.blp',     'data/ui/gnostr-composer.ui'],
    ['data/ui/gnostr-profile-pane.blp', 'data/ui/gnostr-profile-pane.ui'],
    ['data/ui/widgets/gnostr-timeline-view.blp', 'data/ui/widgets/gnostr-timeline-view.ui'],
    ['data/ui/widgets/gnostr-note-embed.blp',    'data/ui/widgets/gnostr-note-embed.ui'],
  ]
  foreach _blp : _blp_sources
    _compiled = custom_target('blp-' + _blp[0].underscorify(),
      input: files(_blp[0]),
      output: _blp[1].split('/')[-1],
      command: [blueprint_compiler, 'compile',
                '--output', meson.current_source_dir() / _blp[1],
                '@INPUT@'])
    nostr_gtk_blueprint_deps += _compiled
  endforeach
endif

# ── GResource ────────────────────────────────────────────────────────
nostr_gtk_resources = gnome.compile_resources(
  'nostr-gtk-resources',
  'data/nostr-gtk.gresource.xml',
  source_dir: 'data',
  c_name: 'nostr_gtk',
  dependencies: nostr_gtk_blueprint_deps)

# ── Source files ─────────────────────────────────────────────────────
# Standalone core: compiled without any apps/gnostr include directory.
nostr_gtk_core_sources = files(
  'src/nostr-gtk-init.c',
  'src/gnostr-composer.c',
  'src/gnostr-card-visibility-policy.c',
  'src/note-card-binding-ctx.c',
  'src/note-card-data.c',
  'src/gn-timeline-tabs.c',
  'src/gnostr-timeline-view.c',
)

# Transitional: widgets that still include apps/gnostr headers and resolve
# Gnostr app symbols at final link time. Kept in the library for ABI; do not
# add sources here (see CMakeLists.txt).
nostr_gtk_gnostr_coupled_sources = files(
  'src/gnostr-thread-view.c',
  'src/gnostr-profile-pane.c',
  'src/nostr-note-card-row.c',
  'src/gnostr-note-embed.c',
)

nostr_gtk_sources = nostr_gtk_core_sources + nostr_gtk_gnostr_coupled_sources
nostr_gtk_portable_sources = files(
  'src/content_renderer.c',
  'src/nostr-gtk-error.c',
  'src/gn-markdown.c',
  'src/gn-markdown-pango.c',
  'src/gn-nostr-reference.c',
  'src/gn-og-preview-card.c',
  'src/gn-media-viewer.c',
  'src/gn-animated-image.c',
  'src/gn-nip34-issue-fields.c',
  'src/gn-portable-i18n.c',
)
nostr_gtk_portable_headers = files(
  'include/nostr-gtk-1.0/content_renderer.h',
  'include/nostr-gtk-1.0/nostr-gtk-error.h',
  'include/nostr-gtk-1.0/gn-markdown.h',
  'include/nostr-gtk-1.0/gn-markdown-pango.h',
  'include/nostr-gtk-1.0/gn-nostr-reference.h',
  'include/nostr-gtk-1.0/gn-og-preview-card.h',
  'include/nostr-gtk-1.0/gn-media-viewer.h',
  'include/nostr-gtk-1.0/gn-animated-image.h',
  'include/nostr-gtk-1.0/gn-nip34-issue-fields.h',
  'include/nostr-gtk-1.0/gn-portable-i18n.h',
)

# ── Header files (public API) ───────────────────────────────────────
nostr_gtk_headers = files(
  'include/nostr-gtk-1.0/nostr-gtk.h',
  'include/nostr-gtk-1.0/nostr-gtk-error.h',
  'include/nostr-gtk-1.0/content_renderer.h',
  'include/nostr-gtk-1.0/gnostr-thread-view.h',
  'include/nostr-gtk-1.0/gnostr-composer.h',
  'include/nostr-gtk-1.0/gnostr-note-embed.h',
  'include/nostr-gtk-1.0/gnostr-profile-pane.h',
  'include/nostr-gtk-1.0/gnostr-card-visibility-policy.h',
  'include/nostr-gtk-1.0/nostr-note-card-row.h',
  'include/nostr-gtk-1.0/gn-timeline-tabs.h',
  'include/nostr-gtk-1.0/gnostr-timeline-view.h',
  'include/nostr-gtk-1.0/gn-markdown.h',
  'include/nostr-gtk-1.0/gn-markdown-pango.h',
  'include/nostr-gtk-1.0/gn-nostr-reference.h',
  'include/nostr-gtk-1.0/gn-og-preview-card.h',
  'include/nostr-gtk-1.0/gn-media-viewer.h',
  'include/nostr-gtk-1.0/gn-animated-image.h',
  'include/nostr-gtk-1.0/gn-nip34-issue-fields.h',
  'include/nostr-gtk-1.0/gn-portable-i18n.h',
)

# ── Include directories ───────────────────────────────────────────────────────
# Internal: library sources use #include "nostr-gtk.h" (flat)
nostr_gtk_inc = include_directories('include/nostr-gtk-1.0')
# External: consumers use #include <nostr-gtk-1.0/nostr-gtk.h>
nostr_gtk_pub_inc = include_directories('include')

# Repository library headers used by the implementation.
nostr_gtk_repo_inc = include_directories(
  '../libnostr/include',
  '../libnostr/generated',
  '../libgo/include',
  '../third_party/nostrdb/src',
  '../third_party/nostrdb/ccan',
  '../nips/nip19/include',
  '../nips/nip10/include',
  '../nips/nip25/include',
)

# Transitional: apps/gnostr private headers, visible to the coupled sources only.
nostr_gtk_app_inc = include_directories(
  '../apps/gnostr/src',
  '../apps/gnostr/src/ui',
  '../apps/gnostr/src/model',
  '../apps/gnostr/src/util',
)

# ── Collect all dependencies ─────────────────────────────────────────
nostr_gtk_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  gtk4_dep,
  adw_dep,
  nostr_gobject_dep,
]

if json_glib_dep.found()
  nostr_gtk_deps += json_glib_dep
endif

nostr_gtk_compile_args = []
if soup3_dep.found()
  nostr_gtk_deps += soup3_dep
  nostr_gtk_compile_args += ['-DHAVE_SOUP3=1']
endif

nostr_gtk_link_args = []
if host_machine.system() == 'darwin'
  nostr_gtk_link_args += ['-Wl,-undefined,dynamic_lookup']
elif host_machine.system() == 'linux'
  nostr_gtk_link_args += ['-Wl,--allow-shlib-undefined']
endif

# ── Build the libraries ──────────────────────────────────────────────
nip19_lib = cc.find_library('nip19', dirs: [cmake_build_dir / 'nips/nip19'], required: true)
nostrdb_storage_lib = cc.find_library('nostrdb_storage',
  dirs: [cmake_build_dir / 'components/nostrdb'], required: true)
nostrdb_vendored_lib = cc.find_library('nostrdb_vendored',
  dirs: [cmake_build_dir / 'components/nostrdb'], required: true)
nostrdb_lib = cc.find_library('nostrdb',
  dirs: [cmake_build_dir / 'libnostr'], required: true)
nostr_lib = cc.find_library('nostr',
  dirs: [cmake_build_dir / 'libnostr'], required: true)
nostr_gtk_portable_lib = library('nostr-gtk-portable-1.0',
  nostr_gtk_portable_sources,
  include_directories: [nostr_gtk_inc, nostr_gtk_pub_inc, nostr_gtk_repo_inc],
  dependencies: [glib_dep, gobject_dep, gio_dep, gtk4_dep, adw_dep,
                 json_glib_dep, nostr_gobject_dep, nip19_lib,
                 nostrdb_storage_lib, nostrdb_vendored_lib, nostrdb_lib,
                 nostr_lib],
  link_args: nostr_gtk_link_args,
  c_args: ['-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_6',
           '-DNOSTR_GTK_LOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
           '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_6',
           '-DGTK_VERSION_MIN_REQUIRED=GDK_VERSION_4_6',
           '-DGTK_VERSION_MAX_ALLOWED=GDK_VERSION_4_6'],
  install: true)
nostr_gtk_portable_dep = declare_dependency(
  link_with: nostr_gtk_portable_lib,
  include_directories: nostr_gtk_pub_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep, gtk4_dep, adw_dep])

nostr_gtk_gnostr_coupled = static_library('nostr-gtk-gnostr-coupled',
  nostr_gtk_gnostr_coupled_sources,
  include_directories: [nostr_gtk_inc, nostr_gtk_pub_inc, nostr_gtk_repo_inc,
                        nostr_gtk_app_inc],
  dependencies: nostr_gtk_deps,
  c_args: nostr_gtk_compile_args,
  pic: true,
  install: false)

nostr_gtk_lib = library('nostr-gtk-1.0',
  nostr_gtk_core_sources + nostr_gtk_resources,
  include_directories: [nostr_gtk_inc, nostr_gtk_pub_inc, nostr_gtk_repo_inc],
  link_whole: nostr_gtk_gnostr_coupled,
  dependencies: nostr_gtk_deps + [nostr_gtk_portable_dep],
  c_args: nostr_gtk_compile_args,
  link_args: nostr_gtk_link_args,
  install: true)

# ── Declare dependency for use as subproject ─────────────────────────
nostr_gtk_dep = declare_dependency(
  link_with: nostr_gtk_lib,
  include_directories: nostr_gtk_pub_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep, gtk4_dep, adw_dep, nostr_gtk_portable_dep])

# ── GObject Introspection ────────────────────────────────────────────
if get_option('introspection')
  nostr_gobject_gir_input = cmake_build_dir / 'nostr-gobject' / 'gir' / 'GNostr-1.0.gir'
  subdir('gir-1.0')
  nostr_gobject_gir_dir = meson.current_build_dir() / 'gir-1.0'

  gir_xdg_data_dirs = [meson.current_build_dir()]
  if host_machine.system() == 'darwin'
    gir_xdg_data_dirs += ['/opt/homebrew/share', '/usr/local/share', '/usr/share']
  else
    gir_xdg_data_dirs += ['/usr/local/share', '/usr/share']
  endif
  gir_env = environment()
  gir_env.set('XDG_DATA_DIRS', ':'.join(gir_xdg_data_dirs))

  nostr_gtk_gir = gnome.generate_gir(nostr_gtk_lib,
    sources: nostr_gtk_sources + nostr_gtk_portable_sources + nostr_gtk_headers,
    namespace: 'GNostrGtk',
    nsversion: '1.0',
    symbol_prefix: 'nostr_gtk',
    identifier_prefix: 'NostrGtk',
    include_directories: [nostr_gtk_repo_inc, nostr_gtk_app_inc],
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0', 'Gtk-4.0', 'Adw-1', 'GNostr-1.0'],
    extra_args: [
      '--add-include-path=' + nostr_gobject_gir_dir,
      '--add-include-path=' + (cmake_build_dir / 'nostr-gobject' / 'gir'),
    ],
    env: gir_env,
    install: true)

  # ── VAPI generation (Vala bindings) ────────────────────────────────
  if get_option('vapi')
    gnome.generate_vapi('nostr-gtk-1.0',
      sources: nostr_gtk_gir[0],
      packages: ['glib-2.0', 'gobject-2.0', 'gio-2.0', 'gtk4', 'libadwaita-1', 'GNostr-1.0'],
      gir_dirs: [nostr_gobject_gir_dir, cmake_build_dir / 'nostr-gobject' / 'gir'],
      install: true)
  endif
endif

# ── pkg-config ───────────────────────────────────────────────────────
pkg = import('pkgconfig')
pkg.generate(nostr_gtk_lib,
  name: 'nostr-gtk-1.0',
  description: 'GTK4/libadwaita widgets for Nostr — built on nostr-gobject',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0', 'gtk4', 'libadwaita-1', 'nostr-gtk-portable-1.0'],
  requires_private: ['nostr-gobject-1.0'])

pkg.generate(nostr_gtk_portable_lib,
  name: 'nostr-gtk-portable-1.0',
  description: 'App-independent Nostr parsing and GTK widgets',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0', 'gtk4', 'libadwaita-1'],
  requires_private: ['nostr-gobject-1.0'])

# ── Header installation ─────────────────────────────────────────────
install_headers(nostr_gtk_headers, subdir: 'nostr-gtk-1.0')

# ── Tests ────────────────────────────────────────────────────────────
if get_option('tests')
  subdir('tests')
endif
