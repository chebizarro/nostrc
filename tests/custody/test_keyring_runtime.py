"""Real Linux keyring commands and existing CTest-owned isolation, not tool mocks."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

import test_contained_engine as fixtures

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import test_selection as selection
from test_selection_custody import CustodyError, validate_lifecycle


class ActualKeyringRuntime(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.ActualContainedEngine()
        self.fixture.setUp()
        f = self.fixture
        f.policy['profiles']['contained.command']['capabilities'] = ['gnome_keyring', 'private_bus']
        f.policy['checks']['actual']['requires'] = ['gnome_keyring', 'private_bus']
        script = f.source / 'app/check.py'
        script.write_text(script.read_text().replace("assert os.environ['DISPLAY'] and os.environ['DBUS_SESSION_BUS_ADDRESS']", """assert 'DISPLAY' not in os.environ
assert os.environ['HOME'].startswith('/tmp/nostrc-keyring-')
assert os.environ['DBUS_SESSION_BUS_ADDRESS'].startswith('unix:path=/tmp/contained-bus')
sys.path.insert(0, '/source/scripts')
import test_selection_keyring as keyring
bus = keyring.connection(os.environ['DBUS_SESSION_BUS_ADDRESS'])
assert keyring.roundtrip(bus)['create_read_delete']
bus.close_sync(None)
"""))
        self.commit()

    def commit(self):
        f = self.fixture
        (f.source / 'tests/selection/inventory.json').write_text(json.dumps(f.policy))
        f.git('add', '.')
        f.git('-c', 'core.hooksPath=/dev/null', 'commit', '-qm', 'keyring runtime fixture')
        sha = f.git('rev-parse', 'HEAD').decode().strip()
        f.plan = selection.make_plan(f.policy, [], head=sha, mode='broad', profiles=['contained.command'])
        f.plan_path.write_text(json.dumps(f.plan))

    def run_fixture(self):
        f = self.fixture
        process = subprocess.run(f.command(), capture_output=True, timeout=120)
        path = Path(process.stdout.decode().strip().splitlines()[-1])
        return process, json.loads(path.read_text()), path.parent / 'work'

    def assert_cleanup(self, receipt):
        owner = receipt['contained_lifecycle']['receipt']
        self.assertTrue(owner['cleanup_complete'], owner)
        self.assertEqual(owner['container_id'], owner['removed_container_id'])
        self.assertFalse(owner['cleanup_inspect']['State']['Running'])
        self.assertEqual(owner['cleanup_inspect']['State']['Pid'], 0)
        absent = subprocess.run(['docker', 'inspect', owner['container_id']], capture_output=True)
        self.assertNotEqual(absent.returncode, 0)
        self.assertIn(b'no such', absent.stderr.lower())

    def assert_unlaunched(self, receipt, work):
        self.assert_cleanup(receipt)
        self.assertFalse(receipt['diagnostic_eligible'])
        self.assertFalse((work / 'build/proof.txt').exists())
        self.assertFalse(any(b'engine ready' in p.read_bytes() for p in work.rglob('command-actual.log')))

    def test_actual_command_roundtrip_final_cleanup_and_aggregate_mutations(self):
        f = self.fixture
        process, receipt, work = self.run_fixture()
        self.assertEqual(process.returncode, 0, (process.stdout + process.stderr).decode(errors='replace'))
        self.assert_cleanup(receipt)
        runtime = receipt['backend_receipt']['keyring_runtime']
        self.assertTrue(runtime['probe']['create_read_delete'])
        self.assertTrue(runtime['probe']['session_closed'])
        self.assertEqual(runtime['binding']['source_identity'], f.plan['source_identity'])
        self.assertEqual(runtime['executable'], receipt['backend_receipt']['runtime_capability_evidence']['gnome_keyring'])
        self.assertEqual(json.loads((work / 'execution/keyring-setup.json').read_text()), runtime)
        cleanup = receipt['contained_lifecycle']['keyring_cleanup']
        self.assertEqual(cleanup['status'], 'verified')
        self.assertTrue(cleanup['home_tmpfs_destroyed'])
        self.assertEqual(cleanup['daemon_log_sha256'], hashlib.sha256((work / 'private-keyring.raw').read_bytes()).hexdigest())
        self.assertTrue(selection.aggregate(f.policy, f.plan, [receipt], complete=False)['passed'])
        for mutation in ('missing_runtime', 'missing_cleanup', 'no_roundtrip', 'foreign_source', 'bad_tool', 'fake_home'):
            with self.subTest(mutation=mutation):
                altered = copy.deepcopy(receipt)
                if mutation == 'missing_runtime': del altered['backend_receipt']['keyring_runtime']
                elif mutation == 'missing_cleanup': del altered['contained_lifecycle']['keyring_cleanup']
                elif mutation == 'no_roundtrip': altered['backend_receipt']['keyring_runtime']['probe']['create_read_delete'] = False
                elif mutation == 'foreign_source': altered['backend_receipt']['keyring_runtime']['binding']['source_identity']['sha'] = '0' * 40
                elif mutation == 'bad_tool': altered['backend_receipt']['runtime_capability_evidence']['gnome_keyring']['sha256'] = '0' * 64
                else: altered['backend_receipt']['keyring_runtime']['home_root'] = '/home/developer'
                self.assertFalse(selection.aggregate(f.policy, f.plan, [altered], complete=False)['passed'])
        self.assertFalse(selection.aggregate(f.policy, f.plan, [receipt], complete=True)['passed'])

    def test_failure_retains_real_exit_bytes_and_keyring_cleanup(self):
        self.fixture.mode_plan('fail')
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_cleanup(receipt)
        self.assertEqual(receipt['backend_receipt']['checks']['actual']['exit_code'], 19)
        self.assertTrue(any(b'engine ready \xff\n' in p.read_bytes() for p in work.rglob('command-actual.log')))
        self.assertEqual(receipt['contained_lifecycle']['keyring_cleanup']['status'], 'verified')
        self.assertFalse(receipt['diagnostic_eligible'])

    def test_cancellation_preserves_namespace_cleanup_and_raw_negative_attempt(self):
        f = self.fixture
        f.test_actual_engine_cancellation_retains_final_negative_attempt_and_raw_bytes()
        receipt = json.loads(next((f.root / 'evidence').glob('*/receipt.json')).read_text())
        self.assert_cleanup(receipt)
        self.assertEqual(receipt['contained_lifecycle']['keyring_cleanup']['status'], 'verified')
        self.assertTrue(receipt['contained_lifecycle']['runtime_state']['keyring_runtime']['probe']['create_read_delete'])

    def test_missing_separate_bus_requirement_refuses_before_workload(self):
        self.fixture.policy['checks']['actual']['requires'] = ['gnome_keyring']
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('separately require private_bus', receipt['contained_lifecycle']['runtime_state']['error'])

    def test_missing_declaration_refuses_before_resource_setup(self):
        self.fixture.policy['profiles']['contained.command']['capabilities'] = ['private_bus']
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('not declared', receipt['contained_lifecycle']['runtime_state']['error'])
        self.assertFalse((work / 'private-keyring.raw').exists())

    def test_authored_home_redirection_refuses_before_daemon_or_workload(self):
        self.fixture.policy['profiles']['contained.command']['environment'] = {'HOME': '/home/developer'}
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('redirects isolated keyring resources', receipt['contained_lifecycle']['runtime_state']['error'])
        self.assertFalse((work / 'private-keyring.raw').exists())

    def test_command_bus_redirection_refuses_before_workload(self):
        self.fixture.policy['checks']['actual']['environment'] = {'DBUS_SESSION_BUS_ADDRESS': 'unix:path=/tmp/foreign-bus'}
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('redirects isolated keyring resources', receipt['contained_lifecycle']['runtime_state']['error'])

    def test_actual_missing_executable_does_not_become_daemon_presence_success(self):
        runner = self.fixture.source / 'scripts/contained-runner.py'
        runner.write_text(runner.read_text().replace('        if command_keyring:', "        os.environ['PATH'] = '/nonexistent'\n        if command_keyring:"))
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('executable is missing', receipt['contained_lifecycle']['runtime_state']['error'])

    def test_ancestor_symlink_to_persistent_work_cannot_attest_home_cleanup(self):
        self.directory_escape('root')

    def test_control_directory_symlink_to_persistent_work_cannot_attest_cleanup(self):
        self.directory_escape('control')

    def directory_escape(self, target):
        script = self.fixture.source / 'app/check.py'
        script.write_text(script.read_text().replace("pathlib.Path(sys.argv[1]).write_text('actual command')", """import shutil
root = pathlib.Path(os.environ['HOME']).parent if ESCAPE_ROOT else pathlib.Path(os.environ['GNOME_KEYRING_CONTROL'])
persistent = pathlib.Path('/work/stolen-home')
persistent.mkdir(mode=0o700)
for child in root.rglob('*'):
    if child.is_dir():
        (persistent / child.relative_to(root)).mkdir(mode=0o700, parents=True, exist_ok=True)
shutil.rmtree(root)
root.symlink_to(persistent, target_is_directory=True)
pathlib.Path(sys.argv[1]).write_text('actual command')""".replace('ESCAPE_ROOT', repr(target == 'root'))))
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_cleanup(receipt)
        self.assertFalse(receipt['diagnostic_eligible'])
        self.assertTrue((work / 'build/proof.txt').exists())
        self.assertIn('directory ancestry', receipt['contained_lifecycle']['runtime_state']['error'])
        self.assertFalse(receipt['contained_lifecycle']['runtime_state']['keyring_home_final']['verified'])
        self.assertEqual(receipt['contained_lifecycle']['keyring_cleanup']['status'], 'namespace_only')
        self.assertFalse(receipt['contained_lifecycle']['keyring_cleanup']['home_tmpfs_destroyed'])

    def test_unselected_ctest_home_overlay_does_not_own_command_resources(self):
        f = self.fixture
        profile = f.policy['profiles']['contained.command']
        profile['compiler'] = 'gcc'
        profile['configure_args'] = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug']
        f.policy['nodes']['other'] = dict(owner_paths=['other'], depends_on=[], rationale='unselected fixture')
        f.policy['nodes']['app']['depends_on'] = ['other']
        f.policy['checks']['actual']['build_targets'] = ['other']
        f.policy['path_rules'].append(dict(prefix='other/', nodes=['other'], effect='impact', reason='unselected fixture'))
        f.policy['path_rules'].append(dict(prefix='cmake/', nodes=[], effect='global', reason='export contract'))
        f.policy['checks']['other'] = dict(kind='ctest', profiles=['contained.command'], nodes=['other'],
            cost='ordinary', requirement='blocking', ctest_tests={'contained.command': ['other']})
        (f.source / 'other').mkdir()
        (f.source / 'other/other.c').write_text('#include <stdio.h>\nint main(void) { puts("unselected must not run"); return 42; }\n')
        (f.source / 'cmake').mkdir()
        shutil.copyfile(ROOT / 'cmake/NostrcTestInventory.cmake', f.source / 'cmake/NostrcTestInventory.cmake')
        (f.source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(unselected_fixture C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_executable(other other/other.c)
add_test(NAME other COMMAND other)
set_tests_properties(other PROPERTIES ENVIRONMENT "HOME=/home/unselected")
nostrc_describe_test(other OWNER other TARGETS other)
nostrc_finalize_test_inventory()
''')
        self.commit()
        sha = f.git('rev-parse', 'HEAD').decode().strip()
        f.plan = selection.make_plan(f.policy, [{'status': 'M', 'new_path': 'app/check.py'}], head=sha, mode='affected', profiles=['contained.command'])
        f.plan_path.write_text(json.dumps(f.plan))
        self.assertEqual(f.plan['selected_checks'], ['actual'])
        process, receipt, work = self.run_fixture()
        self.assertEqual(process.returncode, 0, (process.stdout + process.stderr).decode(errors='replace'))
        self.assert_cleanup(receipt)
        self.assertEqual(receipt['checks']['actual']['outcome'], 'passed')
        self.assertNotIn('other', receipt['checks'])
        self.assertTrue(selection.aggregate(f.policy, f.plan, [receipt], complete=False)['passed'])

    def test_live_daemon_without_secret_service_name_is_not_readiness(self):
        provider = self.fixture.source / 'scripts/test_selection_keyring.py'
        provider.write_text(provider.read_text().replace("'--components=secrets'", "'--components=ssh'")
                            .replace('        owner, pid = wait_owner(bus)',
                                '        try:\n            owner, pid = wait_owner(bus, timeout=2)\n'
                                '        except CustodyError:\n            assert daemon.poll() is None, "daemon must still be alive"\n            raise'))
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('did not acquire its private bus name', receipt['contained_lifecycle']['runtime_state']['error'])
        self.assertTrue((work / 'private-keyring.raw').exists())

    def test_existing_private_service_is_not_replaced_or_laundered(self):
        runner = self.fixture.source / 'scripts/contained-runner.py'
        runner.write_text(runner.read_text().replace('            test_selection_keyring.provision(context, state)',
            '            test_selection_keyring.provision(context, state)\n            test_selection_keyring.provision(context, state)'))
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertNotEqual(process.returncode, 0)
        self.assert_unlaunched(receipt, work)
        self.assertIn('already has a Secret Service', receipt['contained_lifecycle']['runtime_state']['error'])
        self.assertTrue(receipt['contained_lifecycle']['runtime_state']['keyring_runtime']['probe']['create_read_delete'])
        self.assertEqual(receipt['contained_lifecycle']['keyring_cleanup']['status'], 'verified')

    def test_ctest_reuses_existing_self_owned_bus_home_service_without_global_setup(self):
        f = self.fixture
        f.policy['profiles']['contained.command']['compiler'] = 'gcc'
        f.policy['profiles']['contained.command']['configure_args'] = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug']
        f.policy['profiles']['contained.command']['capabilities'] = ['gnome_keyring']
        f.policy['checks'] = {'actual': dict(kind='ctest', profiles=['contained.command'], nodes=['app'],
            cost='ordinary', requirement='blocking', expected_outcome='pass', ctest_tests={'contained.command': ['owned-keyring']})}
        (f.source / 'cmake').mkdir()
        shutil.copyfile(ROOT / 'cmake/NostrcTestInventory.cmake', f.source / 'cmake/NostrcTestInventory.cmake')
        f.policy['path_rules'].append(dict(prefix='cmake/', nodes=[], effect='global', reason='fixture contract'))
        for name in ('nostrc-test-bus.c', 'nostrc-test-bus.h', 'nostrc-test-isolation.h'):
            shutil.copyfile(ROOT / 'tests/common' / name, f.source / 'app' / name)
        (f.source / 'app/probe.py').write_text("""import os, sys
sys.path.insert(0, '/source/scripts')
import test_selection_keyring as keyring
assert os.environ['HOME'].startswith('/tmp/')
bus = keyring.connection(os.environ['DBUS_SESSION_BUS_ADDRESS'])
keyring.wait_owner(bus)
assert keyring.roundtrip(bus)['create_read_delete']
bus.close_sync(None)
print('actual self-owned Secret Service roundtrip')
""")
        (f.source / 'app/owned.c').write_text(r'''#include "nostrc-test-bus.h"
#include <glib/gstdio.h>
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  gchar *root = g_strdup(nostrc_test_bus_get_dir(bus));
  const gchar *vars[] = {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"};
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    gchar *dir = g_build_filename(root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0); g_setenv(vars[i], dir, TRUE); g_free(dir);
  }
  gchar *password = g_build_filename(root, "password", NULL);
  g_assert_true(g_file_set_contents(password, "fixture-only", -1, NULL));
  const gchar *daemon[] = {"gnome-keyring-daemon", "--foreground", "--unlock", "--components=secrets", NULL};
  nostrc_test_bus_spawn_supervised(bus, "keyring.raw", password, daemon);
  gchar *probe[] = {"python3", "/source/app/probe.py", NULL};
  gint status = -1;
  g_assert_true(g_spawn_sync(NULL, probe, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  nostrc_test_bus_down(bus);
  g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
  g_print("actual existing fixture cleanup complete\n");
  g_free(password); g_free(root); return 0;
}
''')
        (f.source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.22)
project(keyring_fixture C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
add_executable(owned app/owned.c app/nostrc-test-bus.c)
target_link_libraries(owned PRIVATE PkgConfig::GIO)
add_test(NAME owned-keyring COMMAND owned)
nostrc_describe_test(owned-keyring OWNER app TARGETS owned INPUTS app/probe.py scripts/test_selection_keyring.py REQUIRES gnome_keyring)
nostrc_finalize_test_inventory()
''')
        self.commit()
        process, receipt, work = self.run_fixture()
        self.assertEqual(process.returncode, 0, (process.stdout + process.stderr).decode(errors='replace'))
        self.assert_cleanup(receipt)
        self.assertEqual(receipt['backend_receipt']['required_capabilities'], ['gnome_keyring'])
        self.assertNotIn('keyring_runtime', receipt['contained_lifecycle']['runtime_state'])
        self.assertNotIn('private_bus', receipt['contained_lifecycle']['runtime_state'])
        self.assertFalse((work / 'private-keyring.raw').exists())
        self.assertEqual(receipt['checks']['actual']['outcome'], 'passed')
        logs = b'\n'.join(p.read_bytes() for p in work.rglob('*.log'))
        self.assertIn(b'actual existing fixture cleanup complete', logs)
        self.assertTrue(selection.aggregate(f.policy, f.plan, [receipt], complete=False)['passed'])


if __name__ == '__main__':
    unittest.main()
