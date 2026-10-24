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
import shutil
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
                (work / name).write_text('{}\n')  # trusted write_json bytes for {}
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


FINALIZER = '''import json, os, sys
sys.path.insert(0, sys.argv[1])
from test_selection_services import finalize_native_lane
config = json.loads(open(sys.argv[2]).read())
print('namespace custody apparmor: fixture (unconfined)', file=sys.stderr, flush=True)
try:
    if config['outcome'] == 'cancel':
        raise KeyboardInterrupt('lane execution cancelled by signal 15')
    if config['outcome'] == 'error':
        raise RuntimeError('private service setup failed before resolve')
    sys.exit(config['outcome'])
except BaseException as outcome:
    os._exit(finalize_native_lane(config['work'], config['source'], config['observed'],
                                  config['resolutions'], outcome, int(sys.argv[3])))
'''


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class NativeSealLifecycle(unittest.TestCase):
    """Original PR25 shape: CTest/build receipts claim CMake namelinks by content.

    Groundhog job120755613098 (exit 0) and Core job120755612969 (exit 1) both
    claimed e.g. build/libjson/libnostr-json.so with its versioned target's
    hash; the strict seal then refused every outcome. Content is unchanged.
    """
    def setUp(self):
        sys.path.insert(0, str(ROOT / 'scripts'))
        self.addCleanup(sys.path.remove, str(ROOT / 'scripts'))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def reset(self):
        shutil.rmtree(self.root / 'work')
        shutil.rmtree(self.root / 'source')

    def lane(self, kind='original', receipt=True):
        if (self.root / 'work').exists():
            self.reset()  # a failed subTest must not mask the next case
        work = self.root / 'work'
        work.mkdir()
        source = self.root / 'source'
        source.mkdir()
        lib = work / 'build/libjson'
        lib.mkdir(parents=True)
        self.real = lib / 'libnostr-json.so.1.0.0'
        self.real.write_bytes(b'\x7fELF original shared object\n')
        os.symlink('libnostr-json.so.1.0.0', lib / 'libnostr-json.so.1')
        self.namelink = lib / 'libnostr-json.so'
        os.symlink('libnostr-json.so.1', self.namelink)
        log = work / 'evidence/check.log'
        log.parent.mkdir()
        log.write_bytes(b'original raw \xff\n')
        (work / 'lane.log').write_bytes(b'')
        result = {'receipt_kind': kind, 'complete': False, 'complete_gate_eligible': False,
                  'checks': {'check': {'log': str(log), 'log_sha256': _sha(log)}},
                  'backend_receipt': {'input_audit': {'input_hashes': {
                      str(self.namelink): _sha(self.real), str(self.real): _sha(self.real)}}}}
        resolved = {'resolved': True}
        if receipt:
            import test_selection  # the CLI's actual trusted writer
            test_selection.write_json(work / 'receipt.json', result)
            test_selection.write_json(work / 'resolved.json', resolved)
        return work, source, result, resolved

    def finalize(self, work, source, outcome, observed, resolutions):
        config = self.root / 'finalizer.json'
        config.write_text(json.dumps(dict(work=str(work), source=str(source), outcome=outcome,
                                          observed=observed, resolutions=resolutions)))
        read_fd, write_fd = os.pipe()
        try:
            with (work / 'lane.log').open('ab') as log:
                process = subprocess.run([sys.executable, '-I', '-c', FINALIZER, str(ROOT / 'scripts'),
                                          str(config), str(write_fd)], pass_fds=(write_fd,),
                                         stdout=log, stderr=log, timeout=60)
        finally:
            os.close(write_fd)
        try:
            anchor = os.read(read_fd, 65).decode('ascii')
        finally:
            os.close(read_fd)
        return process.returncode, anchor

    def test_original_namelink_shape_is_a_sealed_symlink_claim(self):
        import test_selection_custody as custody
        work, source, result, _ = self.lane()
        snapshot = custody.retained_files(work, services.NATIVE_ROOTS)
        claims = custody.retained_claims(result, work, services.NATIVE_ROOTS)
        self.assertEqual(snapshot['build/libjson/libnostr-json.so'], {'kind': 'symlink', 'target': 'libnostr-json.so.1'})
        self.assertEqual(claims['build/libjson/libnostr-json.so'], claims['build/libjson/libnostr-json.so.1.0.0'])

    def test_namelink_claim_seals_and_attests_after_exit(self):
        work, source, result, resolved = self.lane()
        anchor = services.seal_native_lane(work, source, result, resolved)
        final = services.attest_native_lane(work, anchor)
        self.assertEqual(final['status'], 'verified')
        self.assertFalse(final['complete'])
        self.assertFalse(final['complete_gate_eligible'])

    def test_sealed_namelink_replacement_retarget_removal_and_tamper_refuse(self):
        def replace(work):
            self.namelink.unlink()
            self.namelink.write_bytes(self.real.read_bytes())  # identical bytes, different kind
        def retarget(work):
            self.namelink.unlink()
            os.symlink('libnostr-json.so.1.0.0', self.namelink)  # same content, different chain
        def remove(work):
            self.namelink.unlink()
        def tamper(work):
            self.real.write_bytes(b'\x7fELF replaced shared object\n')
        for mutate in (replace, retarget, remove, tamper):
            with self.subTest(mutate.__name__):
                work, source, result, resolved = self.lane()
                anchor = services.seal_native_lane(work, source, result, resolved)
                mutate(work)
                with self.assertRaisesRegex(services.ServiceError,
                                            'changed after finalizer seal: .*build/libjson/libnostr-json.so'):
                    services.attest_native_lane(work, anchor)
                self.reset()

    def test_unsupported_or_diverging_link_claims_refuse_before_seal(self):
        for target in ('../libjson/libnostr-json.so.1.0.0', None, 'other.so'):
            with self.subTest(target):
                work, source, result, resolved = self.lane()
                self.namelink.unlink()
                if target == 'other.so':
                    (self.namelink.parent / 'other.so').write_bytes(b'different bytes\n')
                os.symlink(target or str(self.real), self.namelink)
                with self.assertRaisesRegex(services.ServiceError, 'differ from pre-exit seal: 1 path.*'
                                            'libnostr-json.so \\(original receipt hash differs from seal\\)'):
                    services.seal_native_lane(work, source, result, resolved)
                self.assertFalse((work / 'native-seal.json').exists())
                self.reset()

    def test_finalizer_seals_success_nonzero_and_cancellation_originals(self):
        for outcome, code, kind in ((0, 0, 'original'), (1, 1, 'lane_failure_v1'), ('cancel', 130, 'lane_failure_v1')):
            with self.subTest(outcome):
                work, source, result, resolved = self.lane(kind)
                original = (work / 'receipt.json').read_bytes()
                returncode, anchor = self.finalize(work, source, outcome, [result], [resolved])
                log = (work / 'lane.log').read_bytes()
                self.assertEqual(returncode, code, log)
                self.assertEqual(services.attest_native_lane(work, anchor)['status'], 'verified')
                self.assertEqual((work / 'receipt.json').read_bytes(), original)
                self.assertEqual(b'KeyboardInterrupt' in log, outcome == 'cancel')
                # The reported outcome precedes the seal; later lane output refuses.
                with (work / 'lane.log').open('ab') as stream:
                    stream.write(b'late diagnostic\n')
                with self.assertRaisesRegex(services.ServiceError, 'top-level retained evidence changed'):
                    services.attest_native_lane(work, anchor)
                self.reset()

    def test_finalizer_seals_early_failure_without_any_run_receipt(self):
        work, source, _, _ = self.lane(receipt=False)
        returncode, anchor = self.finalize(work, source, 'error', [], [])
        self.assertEqual(returncode, 1)
        self.assertIn(b'private service setup failed before resolve', (work / 'lane.log').read_bytes())
        self.assertEqual(services.attest_native_lane(work, anchor)['status'], 'verified')
        seal = json.loads((work / 'native-seal.json').read_text())
        self.assertNotIn('receipt.json', seal['files'])
        self.assertEqual(seal['source_claims'], {})
        (work / 'receipt.json').write_text('{"receipt_kind": "late"}')
        with self.assertRaisesRegex(services.ServiceError, 'top-level retained evidence changed'):
            services.attest_native_lane(work, anchor)

    def test_finalizer_refuses_replaced_unobserved_or_tampered_originals(self):
        def replaced(work, result):
            (work / 'receipt.json').write_text(json.dumps(dict(result, receipt_kind='replaced')))
            return 0, [result], 'native output differs from original in-memory receipt'
        def unobserved(work, result):
            return 1, [], 'published without an original in-memory observation'
        def tampered(work, result):
            self.real.write_bytes(b'\x7fELF replaced before seal\n')
            return 0, [result], 'libnostr-json.so \\(original receipt hash differs from seal\\)'
        for case in (replaced, unobserved, tampered):
            with self.subTest(case.__name__):
                work, source, result, resolved = self.lane()
                outcome, observed, message = case(work, result)
                returncode, anchor = self.finalize(work, source, outcome, observed, [resolved])
                self.assertEqual(returncode, 1)
                self.assertEqual(anchor, '')
                self.assertRegex((work / 'lane.log').read_text(errors='replace'), message)
                self.assertFalse((work / 'native-seal.json').exists())
                with self.assertRaisesRegex(services.ServiceError, 'trusted native finalizer seal missing'):
                    services.attest_native_lane(work, anchor)
                self.reset()


    def test_trusted_serialization_is_exactly_the_cli_write_json_bytes(self):
        import test_selection
        _, _, result, resolved = self.lane(receipt=False)
        cancelled = dict(result, receipt_kind='lane_failure_v1', job_conclusion='cancelled',
                         failure={'errors': [{'type': 'KeyboardInterrupt', 'message': 'signal 15'}]})
        unusual = {'z': [1, 2.5, -0.0, 10 ** 30, {}], 'a': 'café   \U0001f600', 'n': None, 't': True}
        for index, value in enumerate((result, resolved, cancelled, unusual)):
            with self.subTest(index):
                target = self.root / f'writer-{index}.json'
                test_selection.write_json(target, value)
                self.assertEqual(services._trusted_output_bytes(value), target.read_bytes())

    def test_pre_seal_raw_identity_refuses_non_original_bytes(self):
        """Inherited json.loads equality admitted all JSON-equal cases below."""
        import test_selection
        def canonical(value):
            target = self.root / 'canonical.json'
            test_selection.write_json(target, value)
            return target.read_bytes()
        def rewrite(result):
            return result, json.dumps(result).encode(), True
        def unsorted(result):
            return result, (json.dumps(result, indent=2) + '\n').encode(), True
        def bool_int(result):
            return result, canonical(dict(result, complete_gate_eligible=0, complete=0)), True
        def duplicate(result):
            raw = canonical(result)
            return result, raw.replace(b'{\n', b'{\n  "complete": true,\n', 1), True
        def nonfinite_disk(result):
            observed = dict(result, elapsed=1.5)
            return observed, canonical(observed).replace(b'1.5', b'NaN'), False
        def nonfinite_observation(result):
            observed = dict(result, elapsed=float('nan'))
            return observed, (json.dumps(observed, sort_keys=True, indent=2) + '\n').encode(), False
        def trailing(result):
            return result, canonical(result) + b'\n', True
        def truncated(result):
            return result, canonical(result)[:-1], True
        def bom(result):
            return result, b'\xef\xbb\xbf' + canonical(result), False
        cases = {rewrite: 'native output differs', unsorted: 'native output differs', bool_int: 'native output differs',
                 duplicate: 'native output differs', nonfinite_disk: 'native output differs',
                 nonfinite_observation: 'not trusted-writer serializable', trailing: 'native output differs',
                 truncated: 'native output differs', bom: 'native output differs'}
        for case, message in cases.items():
            with self.subTest(case.__name__):
                work, source, result, resolved = self.lane()
                observed, raw, json_equal = case(result)
                (work / 'receipt.json').write_bytes(raw)
                if json_equal:
                    self.assertEqual(json.loads(raw), observed)  # what the inherited check accepted
                with self.assertRaisesRegex(services.ServiceError, message):
                    services.seal_native_lane(work, source, observed, resolved)
                self.assertFalse((work / 'native-seal.json').exists())
                self.assertFalse((work / 'source-inputs').exists())
                self.reset()

    def test_pre_seal_identity_refuses_invalid_shapes_and_resolution_substitution(self):
        import test_selection
        def non_object(work, result, resolved):
            return [result], resolved, 'not a JSON object'
        def symlink(work, result, resolved):
            elsewhere = self.root / 'elsewhere.json'
            test_selection.write_json(elsewhere, result)
            (work / 'receipt.json').unlink()
            os.symlink(elsewhere, work / 'receipt.json')
            return result, resolved, 'native output differs'
        def directory(work, result, resolved):
            (work / 'receipt.json').unlink()
            (work / 'receipt.json').mkdir()
            return result, resolved, 'native output differs'
        def resolution_bool_int(work, result, resolved):
            test_selection.write_json(work / 'resolved.json', {'resolved': 1})
            self.assertEqual(json.loads((work / 'resolved.json').read_text()), resolved)
            return result, resolved, 'native resolution differs'
        for case in (non_object, symlink, directory, resolution_bool_int):
            with self.subTest(case.__name__):
                work, source, result, resolved = self.lane()
                observed, observed_resolution, message = case(work, result, resolved)
                with self.assertRaisesRegex(services.ServiceError, message):
                    services.seal_native_lane(work, source, observed, observed_resolution)
                self.assertFalse((work / 'native-seal.json').exists())
                self.reset()

    def test_seal_pins_trusted_bytes_not_a_swap_after_comparison(self):
        work, source, result, resolved = self.lane()
        native_files = services._native_files
        def swap_then_hash(path):
            (work / 'receipt.json').write_text(json.dumps(result))  # JSON-equal, not original bytes
            return native_files(path)
        with patch.object(services, '_native_files', swap_then_hash):
            with self.assertRaisesRegex(services.ServiceError, 'original output changed while sealing'):
                services.seal_native_lane(work, source, result, resolved)
        self.assertFalse((work / 'native-seal.json').exists())


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

    def test_exit_before_readiness_is_classified_and_reaped(self):
        with self.fake('exit'):
            with self.assertRaisesRegex(services.ServiceError, 'exited before readiness'):
                with services.private_services(['private_bus'], self.evidence):
                    self.fail('exited service admitted')
        _, service, _ = self.final()
        self.assertEqual(service['readiness']['outcome'], 'exited_before_readiness')
        # The kernel closes the exiting task's descriptors (readiness EOF) before
        # the task becomes waitable, so this instantaneous non-reaping probe may
        # legitimately see either state. It must be an observed bool, never None.
        self.assertIn(service['readiness']['service_alive_at_failure'], (False, True))
        self.assertEqual(service['readiness']['late_record_bytes'], 0)
        self.assertEqual(service['exit_code'], 3)  # its own exit, observed by the actual reap
        self.assertTrue(service['reaped'])
        self.assertNotIn('forced_exit', service)

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
