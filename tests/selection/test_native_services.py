"""Direct real private-resource tests; Linux skips are explicit, not acceptance."""
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import signal
import stat
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('native_services', ROOT / 'scripts/test_selection_services.py')
services = importlib.util.module_from_spec(spec)
spec.loader.exec_module(services)


class NativeServicesContracts(unittest.TestCase):
    def test_unknown_resource_never_launches(self):
        with tempfile.TemporaryDirectory() as evidence, patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'unsupported private resource'):
                with services.private_services(['signer_endpoint'], evidence):
                    self.fail('unsupported setup admitted')
            launch.assert_not_called()

    def test_host_process_cannot_mount_private_resources(self):
        code = '''import sys, tempfile
sys.path.insert(0, sys.argv[1])
from test_selection_services import prepare_resources, ServiceError
with tempfile.TemporaryDirectory() as resources:
    try:
        prepare_resources(resources, True)
    except ServiceError as exc:
        assert 'namespace init' in str(exc), str(exc)
    else:
        raise AssertionError('ordinary process admitted to mount setup')
'''
        result = subprocess.run([sys.executable, '-B', '-c', code, str(ROOT / 'scripts')], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_namespace_setup_assertion_does_not_replace_real_mount(self):
        with tempfile.TemporaryDirectory() as resources, patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'kernel tmpfs mount|require Linux'):
                with services.namespace_services(['private_bus'], Path(resources) / 'evidence', resources):
                    self.fail('ordinary host directory admitted as namespace resource mount')
            launch.assert_not_called()

    def test_trusted_seal_digest_does_not_reopen_mutable_seal(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory) / 'work'; work.mkdir()
            source = Path(directory) / 'source'; source.mkdir()
            for name in ('receipt.json', 'resolved.json'):
                (work / name).write_text('{}')
            original = Path.open
            @contextlib.contextmanager
            def replace_after_write(path, *args, **kwargs):
                with original(path, *args, **kwargs) as stream:
                    yield stream
                if path.name == 'native-seal.json' and args == ('xb',):
                    path.write_bytes(b'{"attacker": true}\n')
            sys.path.insert(0, str(ROOT / 'scripts'))
            try:
                with patch.object(Path, 'open', replace_after_write):
                    anchored = services.seal_native_lane(work, source, {}, {})
                self.assertNotEqual(anchored, hashlib.sha256((work / 'native-seal.json').read_bytes()).hexdigest())
                with self.assertRaisesRegex(services.ServiceError, 'seal bytes changed'):
                    services.attest_native_lane(work, anchored)
            finally:
                sys.path.pop(0)

    def test_readiness_eof_refuses(self):
        read_fd, write_fd = os.pipe()
        os.close(write_fd)
        try:
            with self.assertRaisesRegex(services.ServiceError, 'before readiness'):
                services._ready(read_fd)
        finally:
            os.close(read_fd)


    def test_late_readiness_drain_is_bounded_and_nonblocking(self):
        read_fd, write_fd = os.pipe()
        try:
            self.assertEqual(services._drain_late_readiness(read_fd), b'')  # writer open: no hang
            os.write(write_fd, b'7\n' + b'x' * 10)
            self.assertEqual(services._drain_late_readiness(read_fd, limit=4), b'7\nxx')
        finally:
            os.close(read_fd)
            os.close(write_fd)

    def test_readiness_failure_classification(self):
        self.assertEqual(services._readiness_outcome(
            services.ServiceError('private service readiness deadline exceeded')), 'deadline_exceeded')
        self.assertEqual(services._readiness_outcome(
            services.ServiceError('private service exited before readiness')), 'exited_before_readiness')
        self.assertEqual(services._readiness_outcome(KeyboardInterrupt()), 'cancelled')
        self.assertEqual(services._readiness_outcome(OSError('other')), 'error')


@unittest.skipUnless(platform.system() == 'Linux', 'actual Linux private services; not native macOS acceptance')
class NativeServicesActual(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='nostrc-service-tests-')
        self.addCleanup(self.temporary.cleanup)
        self.evidence = Path(self.temporary.name) / 'evidence'
        self.before = dict(os.environ)

    def check_final(self, status):
        self.assertEqual(dict(os.environ), self.before)
        result = json.loads((self.evidence / 'services.json').read_text())
        self.assertEqual(result['status'], status, result)
        self.assertFalse(result['complete'])
        self.assertTrue(result['private_home_removed'], result)
        self.assertTrue(all(row['reaped'] for row in result['services']), result)
        for value in result['executables'].values():
            self.assertTrue(Path(value['path']).is_absolute())
            self.assertEqual(len(value['sha256']), 64)
        return result

    def test_real_private_bus_display_and_environment_restoration(self):
        with services.private_services(['private_bus', 'display'], self.evidence) as state:
            self.assertTrue(state['ready'])
            self.assertNotEqual(os.environ['HOME'], self.before.get('HOME'))
            self.assertEqual(Path(os.environ['XDG_RUNTIME_DIR']).stat().st_mode & 0o777, 0o700)
            command = subprocess.run(['dbus-send', '--session', '--print-reply',
                '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus.ListNames'], capture_output=True)
            self.assertEqual(command.returncode, 0, command.stderr)
            self.assertIn(b'org.freedesktop.DBus', command.stdout)
            self.assertNotIn(b'org.freedesktop.secrets', command.stdout)
            self.assertTrue(os.environ['DISPLAY'].startswith(':'))
            self.assertTrue(Path(os.environ['XAUTHORITY']).is_file())
            services._display_ready(os.environ['DISPLAY'][1:],
                subprocess.check_output(['xauth', 'list']).decode().splitlines()[-1].split()[-1])
            with self.assertRaisesRegex(services.ServiceError, 'authentication failed'):
                services._display_ready(os.environ['DISPLAY'][1:], '00' * 16)
        self.check_final('passed')

    def test_backend_failure_retains_first_error_and_tears_down_services(self):
        with self.assertRaisesRegex(ValueError, 'backend negative'):
            with services.private_services(['private_bus', 'display'], self.evidence):
                raise ValueError('backend negative')
        result = self.check_final('failed')
        self.assertEqual(result['error'], 'ValueError: backend negative')

    def test_backend_cancellation_finalizer_remains_in_current_process(self):
        with self.assertRaises(KeyboardInterrupt):
            with services.private_services(['private_bus'], self.evidence):
                raise KeyboardInterrupt('actual finalizer path')
        result = self.check_final('failed')
        self.assertIn('KeyboardInterrupt', result['error'])

    def test_missing_setup_executable_refuses_before_any_service(self):
        with patch.dict(os.environ, {'PATH': str(self.evidence)}, clear=False), patch.object(services.subprocess, 'Popen') as launch:
            with self.assertRaisesRegex(services.ServiceError, 'executable unavailable'):
                with services.private_services(['private_bus'], self.evidence):
                    self.fail('missing tool admitted')
            launch.assert_not_called()


# Xserver-like fake bus service: SIGPIPE ignored, SIGTERM only flags shutdown,
# initialization continues, then the readiness record is written to the
# authored descriptor (dbus --print-address / Xvfb -displayfd semantics).
FAKE_SERVICE = """#!{python}
import json, os, signal, sys, time
fd = int(next(a for a in sys.argv if a.startswith('--print-address=')).split('=', 1)[1])
mode = {mode!r}
links = {{}}
for name in os.listdir('/proc/self/fd'):
    try:
        links[name] = os.readlink('/proc/self/fd/' + name)
    except OSError:
        pass  # the listing's own transient directory descriptor
print('fds ' + json.dumps(links), flush=True)
if mode == 'exit':
    sys.exit(3)
signal.signal(signal.SIGPIPE, signal.SIG_IGN)
if mode == 'late':
    signal.signal(signal.SIGTERM, lambda *_: print('term during init', flush=True))
    time.sleep(1.5)
if mode in ('late', 'ready'):
    try:
        os.write(fd, b'unix:abstract=nostrc-fake-late\\n')
        print('readiness write ok', flush=True)
    except OSError as exc:
        print('Cannot write readiness to fd', fd, exc.errno, flush=True)
        sys.exit(1)
    os.close(fd)
if mode in ('late',):
    sys.exit(0)
signal.signal(signal.SIGTERM, signal.SIG_DFL)
while True:
    time.sleep(60)
"""
FAKE_PROBE = "#!/bin/sh\necho 'array [ string \"org.freedesktop.DBus\" ]'\n"


@unittest.skipUnless(platform.system() == 'Linux', 'actual Linux readiness FD lifecycle; not native macOS acceptance')
class ReadinessDescriptorLifecycle(unittest.TestCase):
    """nostrc-0cfsg.36.21: readiness failure must not close the pipe under a live service."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='nostrc-readiness-tests-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.evidence = self.base / 'evidence'
        self.before = dict(os.environ)
        self.fds_before = self.open_fds()

    @staticmethod
    def open_fds():
        def is_open(fd):
            try:
                os.fstat(fd)
                return True
            except OSError:  # the listing's own, now closed, directory descriptor
                return False
        return {fd for fd in map(int, os.listdir('/proc/self/fd')) if is_open(fd)}

    def fake(self, mode):
        tools = self.base / 'tools'
        tools.mkdir(exist_ok=True)
        service, probe = tools / ('service-' + mode), tools / 'probe'
        service.write_text(FAKE_SERVICE.format(python=sys.executable, mode=mode))
        probe.write_text(FAKE_PROBE)
        for path in (service, probe):
            path.chmod(stat.S_IRWXU)
        paths = {'dbus-daemon': service, 'dbus-send': probe}
        identity = lambda name, search_path=None: {'path': str(paths[name]), 'sha256': '0' * 64}
        return patch.object(services, '_identity', identity)

    def final(self):
        self.assertEqual(dict(os.environ), self.before)
        self.assertEqual(self.open_fds(), self.fds_before, 'readiness descriptor leaked')
        result = json.loads((self.evidence / 'services.json').read_text())
        self.assertEqual(result['status'], 'failed')
        self.assertFalse(result['complete'])
        (service,) = result['services']
        self.assertTrue(service['reaped'], result)
        return result, service, (self.evidence / 'dbus-daemon.log').read_text()

    def test_late_handshake_after_deadline_is_retained_not_manufactured_epipe(self):
        with self.fake('late'), patch.object(services, 'READINESS_SECONDS', 0.5):
            with self.assertRaisesRegex(services.ServiceError, 'readiness deadline exceeded'):
                with services.private_services(['private_bus'], self.evidence):
                    self.fail('late service admitted as ready')
        result, service, log = self.final()
        readiness = service['readiness']
        self.assertEqual(readiness['outcome'], 'deadline_exceeded')
        self.assertTrue(readiness['service_alive_at_failure'])
        self.assertEqual(readiness['late_record'], 'unix:abstract=nostrc-fake-late\n')
        self.assertIn('readiness write ok', log)
        self.assertNotIn('Cannot write', log)
        self.assertEqual(service['exit_code'], 0)
        self.assertNotIn('forced_exit', service)
        self.assertEqual(result['error'], 'ServiceError: private service readiness deadline exceeded')

    def test_exit_before_readiness_is_not_reported_alive(self):
        with self.fake('exit'):
            with self.assertRaisesRegex(services.ServiceError, 'exited before readiness'):
                with services.private_services(['private_bus'], self.evidence):
                    self.fail('exited service admitted')
        _, service, _ = self.final()
        self.assertEqual(service['readiness']['outcome'], 'exited_before_readiness')
        self.assertFalse(service['readiness']['service_alive_at_failure'])
        self.assertEqual(service['readiness']['late_record_bytes'], 0)
        self.assertEqual(service['exit_code'], 3)

    def test_cancellation_during_readiness_stops_service_and_keeps_sibling(self):
        sibling = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.addCleanup(sibling.wait)
        self.addCleanup(sibling.kill)  # this test's own unreaped direct child
        timer = threading.Timer(0.5, signal.pthread_kill, (threading.main_thread().ident, signal.SIGINT))
        with self.fake('never'), patch.object(services, 'READINESS_SECONDS', 30):
            timer.start()
            try:
                with self.assertRaises(KeyboardInterrupt):
                    with services.private_services(['private_bus'], self.evidence):
                        self.fail('cancelled service admitted')
            finally:
                timer.cancel()
        _, service, _ = self.final()
        self.assertEqual(service['readiness']['outcome'], 'cancelled')
        self.assertTrue(service['readiness']['service_alive_at_failure'])
        self.assertEqual(service['exit_code'], -signal.SIGTERM)
        self.assertIsNone(sibling.poll(), 'sibling outside service custody was signalled')

    def test_protected_finalizer_fd_closed_in_service_and_retained_by_backend(self):
        finalizer_read, finalizer_write = os.pipe()
        self.addCleanup(os.close, finalizer_read)
        self.addCleanup(os.close, finalizer_write)
        with self.fake('ready'):
            with services.private_services(['private_bus'], self.evidence, finalizer_fd=finalizer_write) as state:
                self.assertTrue(state['ready'])
                readiness = state['services'][0]['readiness']
                self.assertEqual(readiness['outcome'], 'ready')
                inherited = json.loads((self.evidence / 'dbus-daemon.log').read_text()
                                       .splitlines()[0].removeprefix('fds '))
        os.fstat(finalizer_write)  # still the backend's trusted descriptor
        protected = 'pipe:[%d]' % os.fstat(finalizer_write).st_ino
        self.assertNotIn(protected, inherited.values(), 'protected finalizer pipe reached the service')
        self.assertTrue(inherited[str(readiness['fd'])].startswith('pipe:['), inherited)
        self.assertNotEqual(inherited[str(readiness['fd'])], protected)
        self.assertEqual(self.open_fds(), self.fds_before | {finalizer_read, finalizer_write},
                         'service descriptor leaked')
        result = json.loads((self.evidence / 'services.json').read_text())
        self.assertEqual(result['status'], 'passed')
        self.assertTrue(result['services'][0]['reaped'])
