"""Real outer adapter/kernel custody around in-process private bus/display setup.

Linux runtime cells require the explicitly supported unprivileged namespace
kernel policy. No unavailable primitive is converted to a passing runtime case.
"""
import json
import os
from pathlib import Path
import platform
import shutil
import select
import signal
import socket
import subprocess
import sys
import unittest
import uuid

import test_adapter_custody as custody
import test_local_command_adapter as fixture
from test_native_services import services as private_resources

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(platform.system() == 'Linux', 'Linux PID namespace wrapper custody; native macOS NOT RUN')
class NamespaceWrapperAdapterTests(unittest.TestCase):
    capabilities = ['display', 'private_bus']
    setUpFixture = fixture.LocalCommandAdapterTests.setUp
    write = fixture.LocalCommandAdapterTests.write
    git = fixture.LocalCommandAdapterTests.git
    commit = fixture.LocalCommandAdapterTests.commit
    save_policy = fixture.LocalCommandAdapterTests.save_policy
    gate = fixture.LocalCommandAdapterTests.gate
    backend = fixture.LocalCommandAdapterTests.backend
    def configure_ctest(self):
        command = self.policy['checks'].get('command.actual') if getattr(self, 'mixed_lane', False) else None
        custody.AdapterCustodyTests.configure_ctest(self)
        if command:
            self.policy['checks']['command.actual'] = command
            self.save_policy()
    cancel_tree = custody.AdapterCustodyTests.cancel_tree

    def setUp(self):
        self.setUpFixture()
        if os.environ.get('NOSTRC_CUSTODY_TEST_EVIDENCE'):
            destination = Path(os.environ['NOSTRC_CUSTODY_TEST_EVIDENCE']) / (self._testMethodName + '-' + uuid.uuid4().hex)
            self.addCleanup(shutil.copytree, self.work, destination)
        shutil.copy2(ROOT / 'scripts/test_selection_services.py', self.repo / 'scripts/test_selection_services.py')
        self.policy['profiles']['local.command']['capabilities'] = list(self.capabilities)
        self.save_policy()

    def services_receipt(self, directory):
        files = list(directory.glob('0/lane-*/services/services.json'))
        self.assertEqual(len(files), 1)
        state = json.loads(files[0].read_text())
        self.assertTrue(state['ready'], state)
        self.assertTrue(state['private_home_removed'], state)
        self.assertTrue(all(row['reaped'] for row in state['services']), state)
        expected = ({'dbus-daemon'} if 'private_bus' in self.capabilities else set()) | ({'Xvfb'} if 'display' in self.capabilities else set())
        self.assertEqual({row['executable'] for row in state['services']}, expected)
        self.assertFalse(state['complete'])
        if 'display' in self.capabilities:
            self.assertEqual(state['display_identity']['uid_map'].split(), ['0', str(os.getuid()), '1'])
            self.assertEqual(state['display_identity']['gid_map'].split(), ['0', str(os.getgid()), '1'])
            self.assertFalse(state['display_identity']['shared_lock_created'])
        return state

    def test_wrapped_command_positive(self):
        self.write('app/check.py', 'CAPS = ' + repr(self.capabilities) + '\n' + '''import os, pathlib, subprocess, sys
assert ('DISPLAY' in os.environ) == ('display' in CAPS)
assert ('DBUS_SESSION_BUS_ADDRESS' in os.environ) == ('private_bus' in CAPS)
if 'DISPLAY' in os.environ:
    assert os.environ['DISPLAY'].startswith(':')
    assert pathlib.Path(os.environ['XAUTHORITY']).is_file()
assert 'nostrc-native-services-' in os.environ['HOME']
assert 'NOSTRC_NATIVE_SEAL_FD' not in os.environ
try:
    list(pathlib.Path('/proc/' + str(os.getppid()) + '/fd').iterdir())
except PermissionError:
    pass
else:
    raise AssertionError('workload can inspect trusted finalizer descriptors')
if 'DBUS_SESSION_BUS_ADDRESS' in os.environ:
    p = subprocess.run(['dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.DBus',
        '/org/freedesktop/DBus', 'org.freedesktop.DBus.ListNames'], capture_output=True)
    assert p.returncode == 0, p.stderr
    assert b'org.freedesktop.DBus' in p.stdout
    assert b'org.freedesktop.secrets' not in p.stdout
pathlib.Path(sys.argv[2]).write_text('private resources used\\n')
print('real wrapped command', flush=True)
''')
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns')
        self.assertEqual(receipt['status'], 'partial', receipt)
        self.assertFalse(receipt['complete_gate_eligible'])
        self.assertEqual(self.services_receipt(directory)['status'], 'passed')
        backend = self.backend(receipt)
        self.assertTrue(backend['diagnostic_eligible'], backend)
        self.assertFalse(backend['complete_gate_eligible'])
        # The adapter has already removed its materialized checkout. Retained
        # source bytes must still permit independent partial aggregation.
        public_path = next(directory.glob('0/lane-*/native-receipt.json'))
        public = json.loads(public_path.read_text())
        sys.path.insert(0, str(ROOT / 'scripts'))
        try:
            private_resources.verify_native_public_receipt(public)
            retained = next(public_path.parent.glob('source-inputs/*.input'))
            retained.write_bytes(retained.read_bytes() + b'changed')
            with self.assertRaisesRegex(private_resources.ServiceError, 'retained'):
                private_resources.verify_native_public_receipt(public)
        finally:
            sys.path.pop(0)

    def test_wrapped_nonzero_preserves_raw_bytes_and_backend_receipt(self):
        self.write('app/check.py', "import os, sys\nos.write(1, b'wrapped failure \\xff\\n')\nsys.exit(23)\n")
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns', success=False)
        self.assertEqual(receipt['status'], 'failed', receipt)
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')
        self.assertTrue(any(b'wrapped failure \xff\n' in p.read_bytes() for p in directory.rglob('command-*.log')))
        backend = json.loads(next(directory.glob('0/lane-*/receipt.json')).read_text())
        self.assertFalse(backend['diagnostic_eligible'], backend)
        self.assertFalse(backend['complete_gate_eligible'], backend)

    def sibling_service_roundtrip(self, address):
        result = subprocess.run(['dbus-send', '--print-reply', '--bus=' + address,
            '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus.ListNames'],
            capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'org.freedesktop.DBus', result.stdout)

    def test_wrapped_command_cancel_retains_finalizer_and_sibling(self):
        with private_resources.private_services(['private_bus'], self.work / 'sibling-bus'):
            address = os.environ['DBUS_SESSION_BUS_ADDRESS']
            self.cancel_tree(custody='pidns')
            self.sibling_service_roundtrip(address)
        directory = next((self.work / 'custody-evidence').iterdir())
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')

    def test_wrapped_ctest_cancel_retains_finalizer_and_sibling(self):
        with private_resources.private_services(['private_bus'], self.work / 'sibling-bus'):
            address = os.environ['DBUS_SESSION_BUS_ADDRESS']
            self.cancel_tree(ctest=True, custody='pidns')
            self.sibling_service_roundtrip(address)
        directory = next((self.work / 'custody-evidence').iterdir())
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')

    def test_mixed_command_and_ctest_use_same_private_resources(self):
        command = self.policy['checks']['command.actual']
        self.configure_ctest()
        self.policy['checks']['command.actual'] = command
        self.write('app/check.py', 'CAPS = ' + repr(self.capabilities) + '\n' + '''import os, pathlib, sys
assert ('DISPLAY' in os.environ) == ('display' in CAPS)
assert ('DBUS_SESSION_BUS_ADDRESS' in os.environ) == ('private_bus' in CAPS)
if 'DBUS_SESSION_BUS_ADDRESS' in os.environ:
    assert os.environ['DBUS_SESSION_BUS_ADDRESS'].startswith('unix:')
if 'DISPLAY' in os.environ:
    assert os.environ['DISPLAY'].startswith(':')
assert 'nostrc-native-services-' in os.environ['HOME']
if len(sys.argv) > 2:
    pathlib.Path(sys.argv[2]).write_text('mixed resources used\\n')
print('actual mixed resource consumer', flush=True)
''')
        self.save_policy()
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns')
        self.assertEqual(receipt['status'], 'partial', receipt)
        self.assertEqual(self.services_receipt(directory)['status'], 'passed')
        backend = self.backend(receipt)
        self.assertEqual(set(backend['checks']), {'command.actual', 'ctest.actual'})
        self.assertTrue(backend['diagnostic_eligible'], backend)

    def test_mixed_lane_nonzero_retains_both_selected_check_records(self):
        self.mixed_lane = True
        self.configure_ctest()
        self.write('app/check.py', "import os, sys\nos.write(1, b'mixed negative \\xff\\n')\nsys.exit(23)\n")
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--native-custody', 'pidns', success=False)
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')
        child = json.loads(next(directory.glob('0/lane-*/receipt.json')).read_text())
        self.assertEqual(set(child['checks']), {'ctest.actual', 'command.actual'})
        self.assertFalse(child['diagnostic_eligible'])
        self.assertFalse(child['complete_gate_eligible'])
        self.assertTrue(any(b'mixed negative \xff' in p.read_bytes() for p in directory.rglob('*.log')))

    def test_mixed_lane_cancel_retains_negative_receipt_and_sibling(self):
        self.mixed_lane = True
        with private_resources.private_services(['private_bus'], self.work / 'sibling-bus'):
            address = os.environ['DBUS_SESSION_BUS_ADDRESS']
            self.cancel_tree(ctest=True, custody='pidns')
            self.sibling_service_roundtrip(address)
        directory = next((self.work / 'custody-evidence').iterdir())
        child = json.loads(next(directory.glob('0/lane-*/receipt.json')).read_text())
        self.assertEqual(set(child['checks']), {'ctest.actual', 'command.actual'})
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')

    def test_adapter_death_destroys_service_namespace_and_private_bytes(self):
        endpoint = '/tmp/nostrc-wrap-death-' + uuid.uuid4().hex[:12] + '.sock'
        with socket.socket(socket.AF_UNIX) as server, private_resources.private_services(['private_bus'], self.work / 'sibling-bus'):
            address = os.environ['DBUS_SESSION_BUS_ADDRESS']
            server.bind(endpoint)
            self.addCleanup(os.unlink, endpoint)
            server.listen(1)
            server.settimeout(20)
            self.write('app/check.py', '''import json, os, socket
peer = socket.socket(socket.AF_UNIX)
peer.connect(os.environ['DEATH_READY'])
peer.sendall(json.dumps({'home': os.environ['HOME'], 'bus': os.environ.get('DBUS_SESSION_BUS_ADDRESS')}).encode() + b'\\n')
peer.recv(1)
''')
            head = self.commit()
            evidence = self.work / 'death-evidence'
            process = subprocess.Popen(['bash', str(self.repo / 'scripts/test-gate.sh'), '--repo', str(self.repo),
                '--head', head, '--broad', '--native-custody', 'pidns', '--evidence-dir', str(evidence)],
                env=dict(os.environ, DEATH_READY=endpoint), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            handles = []
            try:
                with server.accept()[0] as peer:
                    peer.settimeout(20)
                    info = json.loads(peer.recv(4096))
                    # Observation only: retain the actual init pidfd before
                    # killing our direct adapter child. Never signal host PIDs.
                    children = Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()
                    self.assertEqual(len(children), 1, children)
                    helper = children[0]
                    init = Path(f'/proc/{helper}/task/{helper}/children').read_text().split()
                    self.assertEqual(len(init), 1, init)
                    handle = os.pidfd_open(int(init[0]))
                    handles.append(handle)
                    status = Path('/proc/' + init[0] + '/status').read_text()
                    self.assertEqual(next(row.split()[-1] for row in status.splitlines() if row.startswith('NSpid:')), '1')
                    process.kill()
                    process.communicate(timeout=20)
                    self.assertEqual(process.returncode, -signal.SIGKILL)
                    self.assertTrue(select.select([handle], [], [], 20)[0], 'namespace init did not reach kernel exit barrier')
                    self.assertEqual(peer.recv(1), b'')
                    self.assertFalse(Path(info['home']).exists(), 'private HOME escaped its tmpfs')
                    if info['bus']:
                        abstract = info['bus'].split('abstract=', 1)[1].split(',', 1)[0]
                        with socket.socket(socket.AF_UNIX) as probe:
                            with self.assertRaises(OSError):
                                probe.connect('\0' + abstract)
                    self.sibling_service_roundtrip(address)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=20)
                for handle in handles:
                    os.close(handle)

    def test_nonempty_resource_mountpoint_refuses_before_target(self):
        resources = self.work / 'invalid-resources'
        resources.mkdir(mode=0o700)
        (resources / 'existing').write_text('must remain unchanged')
        marker = self.work / 'target-must-not-run'
        options = ['--private-display'] if 'display' in self.capabilities else []
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/adapter_namespace_custody.py'),
            '--private-resource-root', str(resources), *options, sys.executable, '-c',
            'import pathlib; pathlib.Path(' + repr(str(marker)) + ').touch()'], capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertIn(b'not empty/owned/private', result.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual((resources / 'existing').read_text(), 'must remain unchanged')

    def test_wrapped_lsan_pass_and_real_leak_failure_without_ptrace(self):
        self.write('app/leak.c', '''#include <stdlib.h>
#include <unistd.h>
__attribute__((noinline)) void allocate(void) { void *p = malloc(1234); if (!p) _exit(12); ((volatile char *)p)[0]=1; }
int main(int argc, char **argv) { (void)argv; if (argc > 1) allocate(); return 0; }
''')
        binary = self.repo / 'app/leak'
        subprocess.run(['cc', '-g', '-O0', '-fsanitize=leak', str(self.repo / 'app/leak.c'), '-o', str(binary)],
                       check=True, capture_output=True, timeout=20)
        check = self.policy['checks']['command.actual']
        check.update(command=['{source}/app/leak'], environment={'LSAN_OPTIONS': 'detect_leaks=1:exitcode=23'},
                     runtime_inputs=['{source}/app/leak.c', '{source}/app/leak'], required_evidence=[])
        self.save_policy()
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--native-custody', 'pidns')
        self.assertEqual(self.services_receipt(directory)['status'], 'passed')
        self.assertFalse(self.backend(receipt)['complete_gate_eligible'])
        check['command'].append('leak')
        self.save_policy()
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--native-custody', 'pidns', success=False)
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')
        raw = b'\n'.join(p.read_bytes() for p in directory.rglob('command-*.log'))
        self.assertIn(b'LeakSanitizer: detected memory leaks', raw)
        self.assertIn(b'1234 byte(s)', raw)
        self.assertNotIn(b'does not work under ptrace', raw)
        failure = json.loads(next(directory.glob('0/lane-*/receipt.json')).read_text())
        self.assertEqual(failure['checks']['command.actual']['exit_code'], 23)
        self.assertFalse(failure['diagnostic_eligible'])
        self.assertFalse(failure['complete_gate_eligible'])

    def configure_build_contract(self, source):
        self.policy['profiles']['local.command'].update(compiler='gcc',
            configure_args=['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug'])
        self.policy['checks']['command.actual'].update(kind='build_contract',
            command=['cmake', '--build', '{build}', '--target', 'probe'], environment={},
            runtime_inputs=[], required_evidence=[])
        target = self.repo / 'cmake/NostrcTestInventory.cmake'
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'cmake/NostrcTestInventory.cmake', target)
        self.write('app/main.c', source)
        self.write('CMakeLists.txt', '''cmake_minimum_required(VERSION 3.22)
project(wrapper_build C)
enable_testing()
include(cmake/NostrcTestInventory.cmake)
add_executable(probe app/main.c)
nostrc_finalize_test_inventory()
''')
        self.save_policy()

    def test_build_contract_real_target_positive(self):
        self.configure_build_contract('int main(void) { return 0; }\n')
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns')
        self.assertEqual(self.services_receipt(directory)['status'], 'passed')
        backend = self.backend(receipt)
        self.assertTrue(backend['input_audit']['after_build'])
        self.assertFalse(backend['complete_gate_eligible'])
        self.assertFalse(backend['complete'])
        self.assertEqual(backend['checks']['command.actual']['exit_code'], 0)

    def test_build_failure_is_retained_without_selected_execution_credit(self):
        self.configure_build_contract('#error deliberate wrapped build negative\n')
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns', success=False)
        self.assertEqual(self.services_receipt(directory)['status'], 'failed')
        failed = json.loads(next(directory.glob('0/lane-*/receipt.json')).read_text())
        self.assertEqual(failed['receipt_kind'], 'lane_failure_v1')
        self.assertEqual(failed['failure']['stage'], 'build')
        self.assertFalse(failed['complete_gate_eligible'])
        self.assertEqual(failed['checks']['command.actual']['observed_executions'], [])
        self.assertTrue(any(b'deliberate wrapped build negative' in p.read_bytes() for p in directory.rglob('build-*.log')))

    def test_keyring_command_setup_still_refuses_before_launch(self):
        self.policy['profiles']['local.command']['capabilities'].append('gnome_keyring')
        self.policy['checks']['command.actual'].setdefault('requires', []).append('gnome_keyring')
        self.save_policy()
        head = self.commit()
        receipt, directory = self.gate('--head', head, '--linux-custody', 'pidns', success=False)
        self.assertIn('gnome_keyring commands require', receipt['executions'][0]['lanes'][0]['error'])
        self.assertFalse(list(directory.rglob('services.json')))
        self.assertFalse(list(directory.rglob('command-*.log')))


class DisplayNamespaceWrapperAdapterTests(NamespaceWrapperAdapterTests):
    capabilities = ['display']


class BusNamespaceWrapperAdapterTests(NamespaceWrapperAdapterTests):
    capabilities = ['private_bus']

    def test_real_late_writer_cannot_promote_original_child_receipt(self):
        endpoint = '/tmp/nostrc-late-' + uuid.uuid4().hex[:16] + '.sock'
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(endpoint)
            self.addCleanup(os.unlink, endpoint)
            server.listen(1)
            server.settimeout(20)
            self.write('app/check.py', '''import ctypes, json, os, pathlib, select, signal, socket, struct, subprocess, sys
if sys.argv[1:2] == ['--late']:
    fd, proof = int(sys.argv[2]), pathlib.Path(sys.argv[3])
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    libc = ctypes.CDLL(None, use_errno=True)
    watch = libc.inotify_init1(os.O_CLOEXEC)
    assert watch >= 0
    assert libc.inotify_add_watch(watch, os.fsencode(proof.parent.parent), 128) >= 0
    peer = socket.socket(socket.AF_UNIX); peer.connect(os.environ['LATE_CONTROL'])
    os.write(fd, b'1'); os.close(fd)
    while True:
        assert select.select([watch], [], [], 15)[0]
        events = os.read(watch, 65536)
        offset = 0
        while offset < len(events):
            wd, mask, cookie, length = struct.unpack_from('=iIII', events, offset)
            name = events[offset+16:offset+16+length].split(b'\\0', 1)[0]
            offset += 16 + length
            if name == b'receipt.json':
                original = json.loads((proof.parent.parent / 'receipt.json').read_text())
                retained = pathlib.Path(original['backend_receipt']['checks']['command.actual']['artifacts']['proof.txt']['path'])
                retained.write_bytes(b'late overwritten artifact\\n')
                peer.sendall(b'attack executed\\n')
                peer.recv(1)
                sys.exit(0)
proof = pathlib.Path(sys.argv[2]); proof.write_bytes(b'original artifact\\n')
response = subprocess.check_output(['dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.DBus',
    '/org/freedesktop/DBus', 'org.freedesktop.DBus.GetConnectionUnixProcessID', 'string:org.freedesktop.DBus'])
service = os.pidfd_open(int(response.split()[-1]))
signal.pidfd_send_signal(service, signal.SIGSTOP); os.close(service)
read_fd, write_fd = os.pipe()
subprocess.Popen([sys.executable, __file__, '--late', str(write_fd), str(proof)],
    pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
os.close(write_fd); assert os.read(read_fd, 1) == b'1'; os.close(read_fd)
os.write(1, b'original raw \\xff\\n')
''')
            head = self.commit()
            evidence = self.work / 'late-evidence'
            process = subprocess.Popen(['bash', str(self.repo / 'scripts/test-gate.sh'), '--repo', str(self.repo),
                '--head', head, '--broad', '--native-custody', 'pidns', '--evidence-dir', str(evidence)],
                env=dict(os.environ, LATE_CONTROL=endpoint), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                with server.accept()[0] as peer:
                    peer.settimeout(20)
                    self.assertEqual(peer.recv(4096), b'attack executed\n')
                    output = process.communicate(timeout=20)[0]
                    self.assertNotEqual(process.returncode, 0, output)
                    self.assertEqual(peer.recv(1), b'', 'late writer survived kernel barrier')
                directory = next(evidence.iterdir())
                child = json.loads(next(directory.glob('0/lane-*/receipt.json')).read_text())
                self.assertTrue(child['diagnostic_eligible'], 'original child result is retained, not rewritten')
                self.assertFalse(list(directory.rglob('native-receipt.json')))
                self.assertTrue(any(b'native original receipt hashes differ' in p.read_bytes() for p in directory.rglob('lane.log')))
                self.assertTrue(any(p.read_bytes() == b'late overwritten artifact\n' for p in directory.rglob('proof.txt')))
                from selector_support import s
                plan = json.loads((directory / '0/plan.json').read_text())
                partial = s.aggregate(self.policy, plan, [child], complete=False, repo=self.repo)
                self.assertFalse(partial['passed'])
                self.assertTrue(any('native kernel custody final evidence refused' in e for e in partial['errors']))
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=20)
