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
import tempfile
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
