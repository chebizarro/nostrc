"""Short owned private-resource placement for UNIX sockets (nostrc-0cfsg.36.5).

Retained evidence roots are deep; dbus-daemon cannot bind `unix:path=` below
them ("Socket name too long"). These tests cover the journaled allocate/release
helper and, on Linux with the supported unprivileged namespace policy, the real
helper/dbus-daemon cells (deep-root negative control, short-root positive,
cancellation and nonzero exit). Unavailable kernel primitives are explicit
skips, never passing runtime evidence; this is not native hosted acceptance.
"""
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from test_native_services import services
import test_namespace_wrapper_adapter as wrapper_tests

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'scripts/adapter_namespace_custody.py'
BASE = os.path.realpath('/tmp')  # short; the macOS TMPDIR cannot fit the socket budget


def deep_directory(parent, depth_chars=140):
    path = Path(parent)
    while len(str(path)) < depth_chars:
        path = path / ('deep-evidence-segment-' + 'x' * 12)
    path.mkdir(parents=True, mode=0o700)
    return path


class PlacementJournalTests(unittest.TestCase):
    def setUp(self):
        self.lane = Path(tempfile.mkdtemp(prefix='placement-lane-'))
        self.addCleanup(shutil.rmtree, self.lane, ignore_errors=True)

    def allocate(self, lane=None):
        path = services.allocate_resource_mountpoint(lane or self.lane, bases=(BASE,))
        self.addCleanup(lambda: path.is_dir() and not os.path.ismount(path) and shutil.rmtree(path, ignore_errors=True))
        return path

    def test_mountpoint_is_short_private_and_independent_of_deep_lane(self):
        lane = deep_directory(self.lane)
        self.assertGreater(len(str(lane)), 120)
        path = self.allocate(lane)
        self.assertLess(len(str(path)) + services.FIXTURE_SOCKET_TAIL, services.UNIX_SOCKET_MAX + 1)
        self.assertEqual(path.parent, Path(BASE))
        self.assertEqual(oct(path.stat().st_mode & 0o777), '0o700')
        self.assertEqual(path.stat().st_uid, os.geteuid())
        self.assertEqual(list(path.iterdir()), [])
        self.assertEqual(services.resource_mountpoint(lane), path)
        records = services.read_resource_journal(lane / services.JOURNAL_NAME)
        self.assertEqual([r['state'] for r in records], ['intent', 'allocated'])
        self.assertEqual(oct((lane / services.JOURNAL_NAME).stat().st_mode & 0o777), '0o600')
        services.require_socket_budget(path)

    def test_deep_root_is_refused_before_any_service_launch(self):
        deep = deep_directory(self.lane)
        with self.assertRaisesRegex(services.ServiceError, 'no room for a UNIX socket path'):
            services.require_socket_budget(deep)

    def test_release_removes_exact_directory_and_is_idempotent(self):
        path = self.allocate()
        self.assertEqual(services.release_resource_mountpoint(self.lane), path)
        self.assertFalse(path.exists())
        self.assertEqual(services.release_resource_mountpoint(self.lane), path)
        records = services.read_resource_journal(self.lane / services.JOURNAL_NAME)
        self.assertEqual([r['state'] for r in records], ['intent', 'allocated', 'released'])
        self.assertTrue(records[-1]['absent_after'])

    def test_release_never_deletes_contents_or_replacements(self):
        path = self.allocate()
        (path / 'residue').write_text('must survive')
        with self.assertRaises(OSError):
            services.release_resource_mountpoint(self.lane)
        self.assertEqual((path / 'residue').read_text(), 'must survive')
        (path / 'residue').unlink()
        path.rmdir()
        path.mkdir(mode=0o700)  # same name, different inode
        with self.assertRaisesRegex(services.ServiceError, 'replaced'):
            services.release_resource_mountpoint(self.lane)
        self.assertTrue(path.is_dir())
        path.rmdir()
        with self.assertRaisesRegex(services.ServiceError, 'vanished'):
            services.release_resource_mountpoint(self.lane)

    def test_release_refuses_a_symlink_swapped_in(self):
        path = self.allocate()
        victim = Path(tempfile.mkdtemp(prefix='placement-victim-'))
        self.addCleanup(shutil.rmtree, victim, ignore_errors=True)
        path.rmdir()
        path.symlink_to(victim)
        with self.assertRaisesRegex(services.ServiceError, 'identity'):
            services.release_resource_mountpoint(self.lane)
        self.assertTrue(victim.is_dir())
        path.unlink()

    def test_journal_is_exclusive_and_tamper_evident(self):
        self.allocate()
        with self.assertRaises(FileExistsError):
            services.allocate_resource_mountpoint(self.lane, bases=(BASE,))
        journal = self.lane / services.JOURNAL_NAME
        lines = journal.read_bytes().splitlines(keepends=True)
        record = json.loads(lines[0]); record['path'] = '/etc'
        journal.write_text(json.dumps(record, sort_keys=True) + '\n' + lines[1].decode())
        with self.assertRaisesRegex(services.ServiceError, 'chain broken'):
            services.read_resource_journal(journal)
        with self.assertRaises(services.ServiceError):
            services.release_resource_mountpoint(self.lane)

    def intent_only(self, lane, token='ab' * 16):
        path = Path(BASE) / (services.RESOURCE_PREFIX + token)
        services._journal_append(lane / services.JOURNAL_NAME,
                                 {'state': 'intent', 'token': token, 'base': BASE, 'path': str(path),
                                  'uid': os.geteuid(), 'pid': 1}, create=True)
        return path

    def states(self, lane):
        return [r['state'] for r in services.read_resource_journal(lane / services.JOURNAL_NAME)]

    def test_intent_only_with_absent_directory_records_absence_without_ownership(self):
        path = self.intent_only(self.lane)
        self.assertFalse(path.exists())
        self.assertEqual(services.release_resource_mountpoint(self.lane), path)
        self.assertEqual(self.states(self.lane), ['intent', 'absent'])
        self.assertFalse(services.read_resource_journal(self.lane / services.JOURNAL_NAME)[-1]['allocation_proved'])
        self.assertEqual(services.release_resource_mountpoint(self.lane), path)  # idempotent
        with self.assertRaisesRegex(services.ServiceError, 'allocated state'):
            services.resource_mountpoint(self.lane)

    def test_intent_only_never_authorizes_removal_of_a_present_directory(self):
        path = self.intent_only(self.lane)
        path.mkdir(mode=0o700)
        self.addCleanup(lambda: path.is_dir() and path.rmdir())
        for _ in range(2):  # the refusal is retained and stays a refusal
            with self.assertRaisesRegex(services.ServiceError, 'cannot authorize|refused earlier'):
                services.release_resource_mountpoint(self.lane)
            self.assertTrue(path.is_dir())
        self.assertEqual(self.states(self.lane), ['intent', 'recovery_refused'])

    def test_mkdir_collision_preserves_the_unowned_directory(self):
        from unittest.mock import patch
        token = 'cd' * 16
        squatter = Path(BASE) / (services.RESOURCE_PREFIX + token)
        squatter.mkdir(mode=0o700)
        self.addCleanup(lambda: squatter.is_dir() and squatter.rmdir())
        with patch.object(services.secrets, 'token_hex', return_value=token), self.assertRaises(FileExistsError):
            services.allocate_resource_mountpoint(self.lane, bases=(BASE,))
        self.assertEqual(self.states(self.lane), ['intent'])
        with self.assertRaises(services.ServiceError):
            services.release_resource_mountpoint(self.lane)
        self.assertTrue(squatter.is_dir())

    def test_allocation_must_agree_with_intent_and_allocator_name(self):
        path = self.allocate()
        records = services.read_resource_journal(self.lane / services.JOURNAL_NAME)
        allocated = records[-1]
        other = Path(BASE) / (services.RESOURCE_PREFIX + 'ef' * 16)
        other.mkdir(mode=0o700)
        self.addCleanup(lambda: other.is_dir() and other.rmdir())
        forged = {
            'path differs from intent': dict(allocated, path=str(other)),
            'token differs from intent': dict(allocated, token='ef' * 16),
            'base differs from intent': dict(allocated, base='/var/tmp'),
        }
        for label, record in forged.items():
            lane = Path(tempfile.mkdtemp(prefix='placement-forged-')); self.addCleanup(shutil.rmtree, lane, ignore_errors=True)
            services._journal_append(lane / services.JOURNAL_NAME, {k: v for k, v in records[0].items() if k != 'prev'}, create=True)
            services._journal_append(lane / services.JOURNAL_NAME, {k: v for k, v in record.items() if k != 'prev'})
            with self.subTest(label), self.assertRaisesRegex(services.ServiceError, 'disagree'):
                services.release_resource_mountpoint(lane)
            with self.assertRaises(services.ServiceError):
                services.resource_mountpoint(lane)
        self.assertTrue(other.is_dir() and path.is_dir())

    def test_non_allocator_name_or_token_is_refused_even_when_self_consistent(self):
        for label, token, name in (('wrong name', 'ab' * 16, 'something-else'),
                                   ('short token', 'ab' * 8, services.RESOURCE_PREFIX + 'ab' * 8),
                                   ('non-hex token', 'zz' * 16, services.RESOURCE_PREFIX + 'zz' * 16)):
            lane = Path(tempfile.mkdtemp(prefix='placement-forged-')); self.addCleanup(shutil.rmtree, lane, ignore_errors=True)
            path = Path(BASE) / name
            path.mkdir(mode=0o700)
            self.addCleanup(lambda p=path: p.is_dir() and p.rmdir())
            info = path.lstat()
            for state in ({'state': 'intent', 'uid': os.geteuid(), 'pid': 1}, {'state': 'allocated', 'dev': info.st_dev, 'ino': info.st_ino}):
                services._journal_append(lane / services.JOURNAL_NAME,
                                         dict(state, token=token, base=BASE, path=str(path)),
                                         create=state['state'] == 'intent')
            with self.subTest(label), self.assertRaisesRegex(services.ServiceError, 'allocator name'):
                services.release_resource_mountpoint(lane)
            self.assertTrue(path.is_dir())

    def test_allocator_uses_the_full_unpredictable_token_in_a_short_name(self):
        path = self.allocate()
        token = services.read_resource_journal(self.lane / services.JOURNAL_NAME)[0]['token']
        self.assertEqual(len(token), 32)
        self.assertEqual(path.name, services.RESOURCE_PREFIX + token)
        services.require_socket_budget(path)

    def test_unusable_bases_fail_closed(self):
        link = self.lane / 'link'
        link.symlink_to(self.lane)
        for index, base in enumerate((str(link), str(self.lane / 'missing'), '/proc')):
            lane = self.lane / ('lane-%d' % index)
            lane.mkdir()
            with self.subTest(base=base), self.assertRaisesRegex(services.ServiceError, 'no short owned directory'):
                services.allocate_resource_mountpoint(lane, bases=(base,))
            self.assertFalse((lane / services.JOURNAL_NAME).exists())

    def test_overlong_base_fails_closed(self):
        deep = deep_directory(self.lane)
        with self.assertRaisesRegex(services.ServiceError, 'no short owned directory'):
            services.allocate_resource_mountpoint(self.lane, bases=(str(deep),))


TARGET = r'''
import json, os, subprocess, sys, tempfile, time
from pathlib import Path
root, mode = Path(sys.argv[1]), sys.argv[2]
private = Path(tempfile.mkdtemp(prefix="nip5f-private-", dir=root))
address = "unix:path=" + str(private / "bus")
config = private / "bus.conf"
config.write_text("<busconfig><type>session</type><listen>" + address + "</listen><auth>EXTERNAL</auth>"
                  "<policy context=\"default\"><allow send_destination=\"*\"/><allow receive_sender=\"*\"/>"
                  "<allow own=\"*\"/></policy></busconfig>")
bus = subprocess.Popen(["dbus-daemon", "--nofork", "--config-file=" + str(config), "--print-address=1"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
report = {"address_len": len(str(private / "bus")), "mount": sys.argv[3] if len(sys.argv) > 3 else ""}
line = bus.stdout.readline().strip()
if not line:
    bus.wait(timeout=10)
    report.update(started=False, exit=bus.returncode, stderr=bus.stderr.read())
    print(json.dumps(report), flush=True); sys.exit(0)
env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=address)
probe = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
                        "--method", "org.freedesktop.DBus.ListNames"], env=env, capture_output=True, timeout=15)
report.update(started=True, roundtrip=probe.returncode == 0 and b"org.freedesktop.DBus" in probe.stdout,
              probe_stderr=probe.stderr.decode(errors="replace"))
print(json.dumps(report), flush=True)
if mode == "hold":
    time.sleep(120)
if mode == "fail":
    sys.exit(7)
bus.terminate(); bus.wait(timeout=10)
'''


@unittest.skipUnless(platform.system() == 'Linux', 'Linux PID/user/mount namespace cells; native macOS NOT RUN')
@unittest.skipUnless(shutil.which('dbus-daemon') and shutil.which('gdbus'), 'dbus-daemon/gdbus unavailable; cells NOT RUN')
class NamespaceSocketPlacementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        probe_root = Path(tempfile.mkdtemp(prefix='placement-probe-'))
        try:
            result = subprocess.run([sys.executable, str(HELPER), '--private-resource-root', str(probe_root),
                                     sys.executable, '-c', 'pass'], capture_output=True, timeout=30)
        finally:
            shutil.rmtree(probe_root, ignore_errors=True)
        if result.returncode:
            raise unittest.SkipTest('supported unprivileged namespace policy unavailable (NOT RUN): '
                                    + result.stderr.decode(errors='replace')[:200])

    def setUp(self):
        self.lane = Path(tempfile.mkdtemp(prefix='placement-lane-'))
        self.addCleanup(shutil.rmtree, self.lane, ignore_errors=True)

    def launch(self, root, mode):
        return subprocess.Popen([sys.executable, str(HELPER), '--private-resource-root', str(root),
                                 sys.executable, '-c', TARGET, str(root), mode],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def report(self, process):
        line = process.stdout.readline()
        self.assertTrue(line, process.stderr.read() if process.poll() is not None else 'no report')
        return json.loads(line)

    def test_deep_resource_root_reproduces_socket_name_too_long(self):
        deep = deep_directory(self.lane, 100)  # fixture socket path then exceeds 107 bytes
        process = self.launch(deep, 'exit')
        report = self.report(process)
        process.communicate(timeout=30)
        self.assertGreater(report['address_len'], services.UNIX_SOCKET_MAX)
        self.assertFalse(report['started'])
        self.assertIn('Socket name too long', report['stderr'])
        self.assertFalse(os.path.ismount(deep))

    def test_short_placement_under_deep_evidence_lane_starts_real_bus(self):
        lane = deep_directory(self.lane)
        root = services.allocate_resource_mountpoint(lane, bases=(BASE,))
        try:
            process = self.launch(root, 'exit')
            report = self.report(process)
            _, stderr = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 0, stderr)
            self.assertLessEqual(report['address_len'], services.UNIX_SOCKET_MAX)
            self.assertTrue(report['started'] and report['roundtrip'], report)
        finally:
            released = services.release_resource_mountpoint(lane)
        self.assertEqual(released, root)
        self.assertFalse(root.exists())

    def test_nonzero_exit_still_releases_namespace_owned_mount(self):
        root = services.allocate_resource_mountpoint(self.lane, bases=(BASE,))
        try:
            process = self.launch(root, 'fail')
            self.assertTrue(self.report(process)['started'])
            process.communicate(timeout=30)
            self.assertEqual(process.returncode, 7)
            self.assertFalse(os.path.ismount(root))
            self.assertEqual(list(root.iterdir()), [])  # tmpfs bytes left with the namespace
        finally:
            services.release_resource_mountpoint(self.lane)
        self.assertFalse(root.exists())

    def test_cancellation_tears_down_bus_and_releases_mountpoint(self):
        root = services.allocate_resource_mountpoint(self.lane, bases=(BASE,))
        try:
            process = self.launch(root, 'hold')
            self.assertTrue(self.report(process)['roundtrip'])
            process.send_signal(signal.SIGTERM)
            started = time.monotonic()
            process.communicate(timeout=60)
            self.assertLess(time.monotonic() - started, 45)
            self.assertEqual(process.returncode, 130)
            self.assertFalse(os.path.ismount(root))
            self.assertEqual(list(root.iterdir()), [])
        finally:
            services.release_resource_mountpoint(self.lane)
        self.assertFalse(root.exists())


@unittest.skipUnless(platform.system() == 'Linux', 'Linux PID namespace public-gate cells; native macOS NOT RUN')
class PublicGatePlacementTests(unittest.TestCase):
    """The real test-gate.sh --linux-custody pidns route with private services."""
    capabilities = ['private_bus']
    for _name in ('setUpFixture', 'write', 'git', 'commit', 'save_policy', 'gate', 'backend',
                  'configure_ctest', 'cancel_tree', 'setUp', 'services_receipt'):
        locals()[_name] = wrapper_tests.NamespaceWrapperAdapterTests.__dict__[_name]
    del _name

    def assert_released_short_placement(self, evidence):
        journals = list(evidence.glob('*/0/lane-*/' + services.JOURNAL_NAME))
        self.assertEqual(len(journals), 1)
        lane = journals[0].parent
        records = services.read_resource_journal(journals[0])
        self.assertEqual([r['state'] for r in records], ['intent', 'allocated', 'released'])
        path = Path(records[-1]['path'])
        self.assertEqual(path.parent, Path('/tmp'))
        self.assertLess(len(str(path)) + services.FIXTURE_SOCKET_TAIL, services.UNIX_SOCKET_MAX + 1)
        self.assertGreater(len(str(lane)), len(str(path)))
        self.assertFalse(path.exists(), 'host mountpoint remained after release')
        self.assertFalse((lane / 'resources').exists())
        services_state = json.loads(next(lane.glob('services/services.json')).read_text())
        self.assertTrue(services_state['private_home_removed'], services_state)
        return path

    def test_positive_run_uses_short_mountpoint_and_releases_it(self):
        self.write('app/check.py', '''import os, pathlib, sys
root = pathlib.Path(os.environ['NOSTRC_NATIVE_RESOURCE_ROOT'])
assert root.parent == pathlib.Path('/tmp') and root.name.startswith('nostrc-rr-'), root
assert len(str(root)) + 27 <= 107
assert 'nostrc-native-services-' in os.environ['HOME']
assert os.environ['HOME'].startswith(str(root) + '/')
pathlib.Path(sys.argv[2]).write_text('short placement\\n')
''')
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns')
        self.assertEqual(self.services_receipt(directory)['status'], 'passed')
        self.assert_released_short_placement(directory.parent)

    def test_nonzero_run_releases_short_mountpoint(self):
        self.write('app/check.py', 'import sys\nsys.exit(23)\n')
        head = self.commit()
        _, directory = self.gate('--head', head, '--linux-custody', 'pidns', success=False)
        self.assert_released_short_placement(directory.parent)

    def test_cancellation_releases_short_mountpoint(self):
        self.cancel_tree(custody='pidns')
        self.assert_released_short_placement(self.work / 'custody-evidence')


class PlacementEnrollmentTests(unittest.TestCase):
    """Exact live-source binding and a finite historical inverse (no golden reset)."""


    def test_changed_or_partial_enrollment_never_resets_goldens(self):
        import copy
        from selector_support import s
        import w11_placement_support as wave
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        data = wave.witness()
        changed = copy.deepcopy(policy)
        changed['checks']['selection.contract']['runtime_inputs'].remove(data['input_additions'][0])
        with self.assertRaises(ValueError):
            wave.before_policy(changed)
        for path in data['sources']:
            with self.assertRaises(ValueError):
                wave.before_source(path, (ROOT / path).read_bytes() + b'\n# unknown change\n')

    def test_services_stay_engine_internal_and_journal_is_the_only_new_gate_state(self):
        gate = (ROOT / 'scripts/test-gate.sh').read_text()
        self.assertIn('allocate_resource_mountpoint(lane_dir)', gate)
        self.assertIn('release_resource_mountpoint(lane_dir)', gate)
        self.assertNotIn('lane_dir / "resources"', gate)
        self.assertNotIn("parent / 'resources'", gate)


if __name__ == '__main__':
    unittest.main()
