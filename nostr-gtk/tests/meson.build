# nostr-gtk/tests/meson.build — Widget tests (Meson, first-class)
#
# All widget tests require headless GTK rendering via xvfb-run on Linux.
# On macOS, GTK can use the native Quartz backend without a display server.
#
# SPDX-License-Identifier: GPL-3.0-or-later

gtk_dep = dependency('gtk4', version: '>=4.6')
adw_dep = dependency('libadwaita-1', version: '>=1.2')
glib_dep = dependency('glib-2.0', version: '>=2.70')
gobject_dep = dependency('gobject-2.0', version: '>=2.70')

# Common test environment
test_env = environment()
test_env.set('G_DEBUG', 'fatal-warnings,gc-friendly')
test_env.set('G_SLICE', 'always-malloc')

# On Linux, use X11 backend for Xvfb
if host_machine.system() == 'linux'
  test_env.set('GDK_BACKEND', 'x11')
endif

# Find xvfb-run for headless rendering (Linux only)
xvfb_run = find_program('xvfb-run', required: host_machine.system() == 'linux')

test_deps = [nostr_gtk_dep, gtk_dep, adw_dep, glib_dep, gobject_dep]

# ── Recycling stress test ────────────────────────────────────────────
test_recycle = executable(
  'test_listview_recycle_stress',
  'test_listview_recycle_stress.c',
  dependencies: test_deps,
)

if xvfb_run.found()
  test('nostr-gtk:listview-recycle-stress', xvfb_run,
    args: ['-a', test_recycle],
    env: test_env,
    timeout: 120,
  )
else
  test('nostr-gtk:listview-recycle-stress', test_recycle,
    env: test_env,
    timeout: 120,
  )
endif

# ── Widget sizing test ───────────────────────────────────────────────
test_measure = executable(
  'test_note_card_measure',
  'test_note_card_measure.c',
  dependencies: test_deps,
)

if xvfb_run.found()
  test('nostr-gtk:note-card-measure', xvfb_run,
    args: ['-a', test_measure],
    env: test_env,
  )
else
  test('nostr-gtk:note-card-measure', test_measure,
    env: test_env,
  )
endif

# ── Widget churn leak test ───────────────────────────────────────────
test_churn = executable(
  'test_widget_churn_leaks',
  'test_widget_churn_leaks.c',
  dependencies: test_deps,
)

if xvfb_run.found()
  test('nostr-gtk:widget-churn-leaks', xvfb_run,
    args: ['-a', test_churn],
    env: test_env,
    timeout: 120,
  )
else
  test('nostr-gtk:widget-churn-leaks', test_churn,
    env: test_env,
    timeout: 120,
  )
endif
