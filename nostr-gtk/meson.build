project('nostr-gtk', 'c',
  version: '1.0.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.59.0',
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

# nostr-gobject-1.0 (sibling library, pre-built)
cmake_build_dir = meson.current_source_dir() / '..' / 'build'

nostr_gobject_lib = cc.find_library('nostr-gobject-1.0',
  dirs: [cmake_build_dir / 'nostr-gobject'],
  required: false)
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

# ── GResource ────────────────────────────────────────────────────────
nostr_gtk_resources = gnome.compile_resources(
  'nostr-gtk-resources',
  'data/nostr-gtk.gresource.xml',
  source_dir: 'data',
  c_name: 'nostr_gtk')

# ── Blueprint support ────────────────────────────────────────────────
blueprint_compiler = find_program('blueprint-compiler', required: false)
nostr_gtk_blueprints = []
if blueprint_compiler.found()
  nostr_gtk_blueprints = custom_target('nostr-gtk-blueprints',
    input: files('data/ui/nostr-note-card-row.blp'),
    output: ['nostr-note-card-row.ui'],
    command: [blueprint_compiler, 'compile',
              '--output', '@OUTPUT0@', '@INPUT0@'])
endif()

# ── Source files ─────────────────────────────────────────────────────
nostr_gtk_sources = files(
  'src/nostr-gtk-init.c',
  'src/gnostr-thread-view.c',
  'src/gnostr-composer.c',
  'src/nostr-note-card-row.c',
)

# ── Header files (public API) ───────────────────────────────────────
nostr_gtk_headers = files(
  'include/nostr-gtk-1.0/nostr-gtk.h',
  'include/nostr-gtk-1.0/gnostr-thread-view.h',
  'include/nostr-gtk-1.0/gnostr-composer.h',
  'include/nostr-gtk-1.0/nostr-note-card-row.h',
)

# ── Include directories ─────────────────────────────────────────────
# Internal: library sources use #include "nostr-gtk.h" (flat)
nostr_gtk_inc = include_directories('include/nostr-gtk-1.0')
# External: consumers use #include <nostr-gtk-1.0/nostr-gtk.h>
nostr_gtk_pub_inc = include_directories('include')

# ── Collect all dependencies ─────────────────────────────────────────
nostr_gtk_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  gtk4_dep,
  adw_dep,
  nostr_gobject_dep,
]

# ── Build the library ────────────────────────────────────────────────
nostr_gtk_lib = library('nostr-gtk-1.0',
  nostr_gtk_sources + nostr_gtk_resources,
  include_directories: [nostr_gtk_inc],
  dependencies: nostr_gtk_deps,
  install: true)

# ── Declare dependency for use as subproject ─────────────────────────
nostr_gtk_dep = declare_dependency(
  link_with: nostr_gtk_lib,
  include_directories: nostr_gtk_pub_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep, gtk4_dep, adw_dep])

# ── GObject Introspection ────────────────────────────────────────────
if get_option('introspection')
  nostr_gtk_gir = gnome.generate_gir(nostr_gtk_lib,
    sources: nostr_gtk_sources + nostr_gtk_headers,
    namespace: 'GNostrGtk',
    nsversion: '1.0',
    symbol_prefix: 'nostr_gtk',
    identifier_prefix: 'NostrGtk',
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0', 'Gtk-4.0', 'Adw-1', 'GNostr-1.0'],
    install: true)

  # ── VAPI generation (Vala bindings) ────────────────────────────────
  if get_option('vapi')
    gnome.generate_vapi('nostr-gtk-1.0',
      sources: nostr_gtk_gir[0],
      packages: ['glib-2.0', 'gobject-2.0', 'gio-2.0', 'gtk4', 'libadwaita-1', 'nostr-gobject-1.0'],
      install: true)
  endif()
endif()

# ── pkg-config ───────────────────────────────────────────────────────
pkg = import('pkgconfig')
pkg.generate(nostr_gtk_lib,
  name: 'nostr-gtk-1.0',
  description: 'GTK4/libadwaita widgets for Nostr — built on nostr-gobject',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0', 'gtk4', 'libadwaita-1'],
  requires_private: ['nostr-gobject-1.0'])

# ── Header installation ─────────────────────────────────────────────
install_headers(nostr_gtk_headers, subdir: 'nostr-gtk-1.0')

# ── Tests ────────────────────────────────────────────────────────────
if get_option('tests')
  # Tests will be added as widgets are moved in
endif()
