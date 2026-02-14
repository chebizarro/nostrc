# tests/testkit/meson.build — Shared test infrastructure library (Meson)
#
# SPDX-License-Identifier: GPL-3.0-or-later

glib_dep = dependency('glib-2.0', version: '>=2.70')
gio_dep  = dependency('gio-2.0', version: '>=2.70')

# nostr_gobject_dep should be declared in nostr-gobject/meson.build
# via declare_dependency(). If building as a subproject, use:
#   nostr_gobject_dep = dependency('nostr-gobject-1.0', fallback: ['nostr-gobject', 'nostr_gobject_dep'])

gnostr_testkit_lib = static_library(
  'gnostr-testkit',
  sources: ['gnostr-testkit.c'],
  include_directories: include_directories('.'),
  dependencies: [glib_dep, gio_dep, nostr_gobject_dep],
)

gnostr_testkit_dep = declare_dependency(
  link_with: gnostr_testkit_lib,
  include_directories: include_directories('.'),
  dependencies: [glib_dep, gio_dep, nostr_gobject_dep],
)
