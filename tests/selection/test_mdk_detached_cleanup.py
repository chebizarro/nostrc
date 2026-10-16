"""F1: real session changes never become a full cleanup attestation.

Docker is fake. Test-owned private connections release descendants without PID
signaling; the helper has no arbitrary-tree ownership mechanism to claim here.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import unittest

import test_mdk_cleanup as fixtures

BUILD, PRELUDE = fixtures.BUILD, fixtures.PRELUDE


class PrivateRelease:
    def __init__(self, directory):
        self.path = str(directory / 'release.sock')
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.connection = None
        self.identity = None

    def accept(self):
        if self.connection is None:
            self.connection, _ = self.listener.accept()
            self.connection.settimeout(5)
            data = bytearray()
            while not data.endswith(b'\n'):
                part = self.connection.recv(4096)
                if not part:
                    raise AssertionError('descendant closed before identity handshake')
                data.extend(part)
            self.identity = json.loads(data)
        return self.identity

    def close(self):
        try:
            self.accept()
            # The descendant is blocked on this exact connection, not a PID
            # guess. EOF releases it, and peer EOF confirms it closed its handle.
            self.connection.shutdown(socket.SHUT_WR)
            if self.connection.recv(1) != b'':
                raise AssertionError('descendant did not finish release handshake')
        finally:
            if self.connection is not None:
                self.connection.close()
            self.listener.close()

    def body(self, detached=True, after=''):
        child = '''import json,os,socket,sys
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[1])
s.sendall(json.dumps(dict(pid=os.getpid(),sid=os.getsid(0),pgid=os.getpgrp())).encode()+b'\\n')
os.write(int(sys.argv[2]),b'R');os.close(int(sys.argv[2]))
assert s.recv(1)==b''
s.close()
'''
        launch = 'start_new_session=True' if detached else 'preexec_fn=lambda: os.setpgid(0,0)'
        return BUILD + f'''
reader,writer=os.pipe()
orphan=subprocess.Popen([sys.executable,'-c',{child!r},{self.path!r},str(writer)],{launch},pass_fds=(writer,),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
os.close(writer)
assert os.read(reader,1)==b'R';os.close(reader)
(state/'grandchild.pid').write_text(str(orphan.pid))
''' + after


class DetachedCleanupTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.LifecycleCleanupTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def release(self):
        release = PrivateRelease(self.fixture.base)
        self.addCleanup(release.close)
        return release

    def assert_no_tree_attestation(self, receipt):
        self.assertFalse(receipt['cleanup_complete'])
        self.assertFalse(receipt['complete'])
        self.assertEqual(receipt['cleanup_results']['arbitrary_process_tree'], 'unverified')
        self.assertIn('session changes/reparenting', receipt['cleanup_limitation'])
        self.assertTrue(receipt['image_results'][0]['build_complete'])
        self.assertEqual(receipt['containers'][0]['exit_code'], 0)

    def test_start_new_session_survivor_forces_truthful_refusal(self):
        release = self.release()
        result, receipt, state = self.fixture.run_guard(release.body())
        identity = release.accept()
        self.assertEqual(identity['pid'], int((state / 'grandchild.pid').read_text()))
        self.assertEqual(os.getsid(identity['pid']), identity['pid'])
        self.assertEqual(identity['sid'], identity['pid'])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(receipt['status'], 'cleanup_unverified')
        self.assertEqual(receipt['command_status'], 'succeeded')
        self.assertEqual(receipt['command_exit_code'], 0)
        self.assertEqual(receipt['cleanup_results']['original_session'], 'quiescent')
        self.assertEqual(receipt['cleanup_results']['recorded_containers'], 'removed')
        self.assert_no_tree_attestation(receipt)

    def test_nonzero_command_is_not_replaced_by_cleanup_unverified(self):
        result, receipt, _ = self.fixture.run_guard(BUILD + '\nsys.exit(37)\n')
        self.assertEqual(result.returncode, 37)
        self.assertEqual(receipt['status'], 'failed')
        self.assertEqual(receipt['command_status'], 'failed')
        self.assertEqual(receipt['command_exit_code'], 37)
        self.assertEqual(receipt['cleanup_results']['original_session'], 'quiescent')
        self.assert_no_tree_attestation(receipt)

    def test_scoped_cleanup_error_does_not_hide_original_command_failure(self):
        release = self.release()
        result, receipt, _ = self.fixture.run_guard(release.body(detached=False, after='\nsys.exit(37)\n'))
        identity = release.accept()
        self.assertEqual(os.getpgid(identity['pid']), identity['pid'])
        self.assertEqual(result.returncode, 37)
        self.assertEqual(receipt['status'], 'failed')
        self.assertEqual(receipt['command_exit_code'], 37)
        self.assertEqual(receipt['cleanup_results']['original_session'], 'unverified')
        self.assertEqual(receipt['cleanup_results']['recorded_containers'], 'removed')
        self.assertIn('owned session descendants remain', ' '.join(receipt['cleanup_errors']))
        self.assert_no_tree_attestation(receipt)

    def test_cancellation_retained_with_detached_descendant(self):
        release = self.release()
        f = self.fixture
        command = [sys.executable, str(f.source / 'scripts/mdk-gate.py'), 'run',
                   '--source-root', str(f.source), '--evidence-dir', str(f.evidence), '--version', '011',
                   '--', sys.executable, '-c', PRELUDE + release.body(after='\nimport signal;signal.pause()\n')]
        helper = subprocess.Popen(command, env=f.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            identity = release.accept()
            self.assertEqual(os.getsid(identity['pid']), identity['pid'])
            helper.terminate()  # Our unreaped direct Popen child, never a guessed PID.
            stdout, stderr = helper.communicate(timeout=15)
            self.assertEqual(helper.returncode, 130, stdout + stderr)
            receipts = list(f.evidence.glob('*/receipt.json'))
            self.assertEqual(len(receipts), 1)
            receipt = json.loads(receipts[0].read_text())
            self.assertEqual(receipt['status'], 'cancelled')
            self.assertEqual(receipt['command_status'], 'cancelled')
            self.assertEqual(receipt['command_exit_code'], -signal.SIGTERM)
            self.assertEqual(os.getsid(identity['pid']), identity['pid'])
            self.assert_no_tree_attestation(receipt)
        finally:
            if helper.poll() is None:
                helper.terminate()
                helper.communicate(timeout=15)


if __name__ == '__main__':
    unittest.main()
