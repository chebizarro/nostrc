"""Real per-CTest AT-SPI custody; Linux fixture evidence, not hosted acceptance."""
import os
import sys
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'linux', 'Linux AT-SPI session launcher')
class PrivateGuiSession(unittest.TestCase):
    def test_real_ctest_cases_have_independent_accessibility_services(self):
        with tempfile.TemporaryDirectory(prefix='nostrc-private-gui-') as directory:
            root = Path(directory)
            for name in ('NostrcTestBus.cmake', 'NostrcTestInventory.cmake', 'NostrcTestIsolation.cmake'):
                dest = root / 'cmake' / name
                dest.parent.mkdir(exist_ok=True)
                shutil.copy(ROOT / 'cmake' / name, dest)
            for name in ('nostrc-test-bus.c', 'nostrc-test-bus-selftest.c', 'nostrc-test-bus.h', 'nostrc-test-isolation.h', 'nostrc-test-session.c'):
                dest = root / 'tests/common' / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(ROOT / 'tests/common' / name, dest)
            (root / 'probe.c').write_text(r'''
#include "nostrc-test-bus.h"
#include <gtk/gtk.h>
#include <signal.h>
int main(int argc, char **argv) {
  g_assert_cmpint(argc, ==, 3);
  const char *runtime = g_getenv("XDG_RUNTIME_DIR"), *address = g_getenv("AT_SPI_BUS_ADDRESS");
  g_assert_nonnull(runtime); g_assert_nonnull(address);
  g_assert_true(g_str_has_prefix(runtime, "/tmp/nostrc-bus-"));
  g_autofree char *outer = g_strdup(g_getenv("DBUS_SESSION_BUS_ADDRESS"));
  /* The accessibility owner is not the test bus whose lifecycle the test exercises. */
  NostrcTestBus *inner = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(inner); nostrc_test_bus_down(inner);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", outer, TRUE);
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) a11y = g_dbus_connection_new_for_address_sync(address,
      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
      NULL, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(a11y);
  g_autoptr(GVariant) id = g_dbus_connection_call_sync(a11y, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetId", NULL, G_VARIANT_TYPE("(s)"), 0, -1, NULL, &error);
  g_assert_no_error(error); g_assert_nonnull(id);
  gtk_init();
  GtkWidget *window = gtk_window_new(); gtk_window_present(GTK_WINDOW(window));
  while (!gtk_widget_get_mapped(window)) g_main_context_iteration(NULL, TRUE);
  GtkATContext *context = gtk_accessible_get_at_context(GTK_ACCESSIBLE(window));
  g_assert_nonnull(context); g_assert_nonnull(strstr(G_OBJECT_TYPE_NAME(context), "AtSpi"));
  g_autofree char *report = g_strdup_printf("%s\n%s\n", runtime, address);
  g_assert_true(g_file_set_contents(argv[1], report, -1, &error)); g_assert_no_error(error);
  gtk_window_destroy(GTK_WINDOW(window));
  g_dbus_connection_close_sync(a11y, NULL, &error); g_assert_no_error(error);
  if (!strcmp(argv[2], "crash")) raise(SIGABRT);
  return !strcmp(argv[2], "fail") ? 42 : 0;
}
''')
            (root / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(private_gui C)
include(CTest)
include(cmake/NostrcTestInventory.cmake)
include(cmake/NostrcTestBus.cmake)
include(cmake/NostrcTestIsolation.cmake)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4)
add_executable(probe probe.c)
target_link_libraries(probe PRIVATE nostrc-test-bus PkgConfig::GTK)
set(NOSTRC_TEST_PRIVATE_SESSION ON)
foreach(name first second)
  add_test(NAME ${name} COMMAND probe "${CMAKE_BINARY_DIR}/${name}.txt" pass)
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "GTK_A11Y=atspi;G_DEBUG=fatal-warnings" TIMEOUT 30)
endforeach()
add_test(NAME external COMMAND ${CMAKE_COMMAND} -E env $<TARGET_FILE:probe> "${CMAKE_BINARY_DIR}/external.txt" pass)
set_tests_properties(external PROPERTIES ENVIRONMENT "GTK_A11Y=atspi;G_DEBUG=fatal-warnings" TIMEOUT 30)
add_test(NAME failed COMMAND probe "${CMAKE_BINARY_DIR}/failed.txt" fail)
set_tests_properties(failed PROPERTIES ENVIRONMENT "GTK_A11Y=atspi;G_DEBUG=fatal-warnings" TIMEOUT 30)
add_test(NAME crashed COMMAND probe "${CMAKE_BINARY_DIR}/crashed.txt" crash)
set_tests_properties(crashed PROPERTIES ENVIRONMENT "GTK_A11Y=atspi;G_DEBUG=fatal-warnings" TIMEOUT 30 WILL_FAIL TRUE)
nostrc_isolate_tests(homes)
# Instrument the launcher, not stock GTK or its out-of-process services.
target_compile_options(nostrc-test-session PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(nostrc-test-session PRIVATE -fsanitize=address,undefined)
# Later descriptor authors may replace the target list; the launcher remains.
set_property(TEST first PROPERTY NOSTRC_SELECTION_TARGETS probe)
nostrc_finalize_test_inventory()
''')
            build = root / 'build'
            def run(*args):
                return subprocess.run(args, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for command in (('cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja'),
                            ('cmake', '--build', str(build), '--target', 'probe')):
                result = run(*command)
                self.assertEqual(result.returncode, 0, result.stdout)
            descriptors = json.loads(next(build.glob('nostrc-test-descriptors-*.json')).read_text())
            first_record = next(test for test in descriptors['tests'] if test['name'] == 'first')
            self.assertIn('nostrc-test-session', first_record['targets'])
            # An inherited address must never redirect these tests to another session.
            env = dict(os.environ, AT_SPI_BUS_ADDRESS='unix:path=/nonexistent/inherited-a11y')
            result = subprocess.run(['xvfb-run', '-a', 'ctest', '--test-dir', str(build), '-j2', '-R', '^(first|second|external)$',
                                     '--output-on-failure'], env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            reports = [(build / (name + '.txt')).read_text().splitlines() for name in ('first', 'second', 'external')]
            self.assertEqual(len({report[0] for report in reports}), 3)
            self.assertEqual(len({report[1] for report in reports}), 3)
            for runtime, address in reports:
                self.assertFalse(Path(runtime).exists())
            failed = run('xvfb-run', '-a', 'ctest', '--test-dir', str(build), '-R', '^failed$', '--output-on-failure')
            self.assertNotEqual(failed.returncode, 0, failed.stdout)
            self.assertIn('failed', failed.stdout)
            self.assertNotIn('LeakSanitizer', failed.stdout)
            self.assertNotIn('runtime error:', failed.stdout)
            self.assertFalse(Path((build / 'failed.txt').read_text().splitlines()[0]).exists())
            crashed = run('xvfb-run', '-a', 'ctest', '--test-dir', str(build), '-R', '^crashed$', '--output-on-failure')
            self.assertNotEqual(crashed.returncode, 0, "WILL_FAIL must not turn a child crash into a pass: " + crashed.stdout)
            self.assertFalse(Path((build / 'crashed.txt').read_text().splitlines()[0]).exists())
