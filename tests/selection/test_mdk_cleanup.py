"""Standalone lifecycle regressions: fake Docker, real host processes/CTest.

These do not admit guarded selector execution or establish live peer acceptance.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest

from test_mdk_gate import FAKE

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = '''
import json,os,pathlib,subprocess,sys
state=pathlib.Path(os.environ['NOSTRC_MDK_STATE'])
config=json.loads((state/'config.json').read_text())
'''
BUILD = '''
for data in config['images'].values():
    subprocess.run([os.environ['NOSTRC_MDK_DOCKER'],'build','-t',data['tag'],data['context']],check=True)
    subprocess.run([os.environ['NOSTRC_MDK_DOCKER'],'run','--rm','-i',data['tag']],check=True)
'''


class LifecycleCleanupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.source = self.base / 'source'
        scripts = self.source / 'scripts'
        scripts.mkdir(parents=True)
        shutil.copyfile(ROOT / 'scripts/mdk-gate.py', scripts / 'mdk-gate.py')
        driver = self.source / 'tests/interop/mdk/driver-0.11'
        driver.mkdir(parents=True)
        (driver / 'Dockerfile').write_text('fake context')
        tools = self.base / 'bin'
        tools.mkdir()
        (tools / 'docker').write_text(FAKE)
        (tools / 'docker').chmod(0o700)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'], TRACE=str(self.base / 'trace'))
        self.evidence = self.base / 'evidence'

    def run_guard(self, body):
        result = subprocess.run([
            sys.executable, str(self.source / 'scripts/mdk-gate.py'), 'run',
            '--source-root', str(self.source), '--evidence-dir', str(self.evidence), '--version', '011',
            '--', sys.executable, '-c', PRELUDE + body,
        ], env=self.env, capture_output=True, text=True, timeout=15)
        receipts = list(self.evidence.glob('*/receipt.json'))
        self.assertEqual(len(receipts), 1, result.stderr)
        return result, json.loads(receipts[0].read_text()), receipts[0].parent

    def test_existing_ctest_fixture_chain_and_receipt_identity(self):
        body = '''
case=state/'ctest';case.mkdir()
data=config['images']['011'];wrapper=os.environ['NOSTRC_MDK_DOCKER']
def register(name,args):
    return 'add_test('+name+' '+ ' '.join('"'+a+'"' for a in args)+')\\n'
(case/'CTestTestfile.cmake').write_text(
    register('setup',[wrapper,'build','-t',data['tag'],data['context']])+
    'set_tests_properties(setup PROPERTIES FIXTURES_SETUP image)\\n'+
    register('consumer',[wrapper,'run','--rm','-i',data['tag']])+
    'set_tests_properties(consumer PROPERTIES FIXTURES_REQUIRED image)\\n')
subprocess.run(['ctest','--test-dir',str(case),'--output-on-failure','--no-tests=error','-R','^consumer$'],check=True)
'''
        result, receipt, state = self.run_guard(body)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('2/2 Test', result.stdout)
        self.assertEqual(receipt['status'], 'cleanup_unverified')
        self.assertEqual(receipt['command_status'], 'succeeded')
        self.assertEqual(receipt['command_exit_code'], 0)
        self.assertFalse(receipt['cleanup_complete'])
        self.assertEqual(receipt['cleanup_results'], dict(original_session='quiescent',
            recorded_containers='removed', arbitrary_process_tree='unverified'))
        self.assertFalse(receipt['complete'])
        config = json.loads((state / 'config.json').read_text())
        canonical = json.dumps(config, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()
        self.assertEqual(receipt['config_digest'], hashlib.sha256(canonical).hexdigest())
        for key in ('helper', 'wrapper'):
            identity = receipt[key]
            self.assertEqual(identity['sha256'], hashlib.sha256(Path(identity['path']).read_bytes()).hexdigest())
        self.assertEqual(receipt['configure_args'], json.loads((state / 'configure-args.json').read_text()))

    def assert_stopped(self, pid):
        status = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='], capture_output=True, text=True).stdout.strip()
        self.assertTrue(not status or status.startswith('Z'), 'owned grandchild survived cleanup')

    @staticmethod
    def clean_known_descendant(pid, session):
        # The session belongs only to this test; never name-match or signal a
        # reused PID in another session, and never touch a sibling's session.
        try:
            if os.getsid(pid) == session:
                os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def test_exited_direct_child_does_not_leave_grandchild_or_kill_sibling(self):
        sibling = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(60)'], start_new_session=True)
        try:
            body = BUILD + '''
orphan=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
(state/'grandchild.pid').write_text(str(orphan.pid))
'''
            result, receipt, state = self.run_guard(body)
            pid = int((state / 'grandchild.pid').read_text())
            try:
                self.assert_stopped(pid)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(receipt['status'], 'cleanup_unverified')
                self.assertEqual(receipt['command_exit_code'], 0)
                self.assertFalse(receipt['cleanup_complete'])
                self.assertEqual(receipt['cleanup_results']['original_session'], 'quiescent')
                self.assertEqual(receipt['cleanup_results']['arbitrary_process_tree'], 'unverified')
                self.assertIsNone(sibling.poll(), 'sibling process was killed')
            finally:
                self.clean_known_descendant(pid, receipt['child_pid'])
        finally:
            sibling.terminate()
            sibling.wait(timeout=5)

    def test_unconfirmed_process_cleanup_still_removes_owned_containers(self):
        body = BUILD + '''
import select
reader,writer=os.pipe()
code="import os,sys,time;os.setpgid(0,0);os.write(int(sys.argv[1]),b'R');time.sleep(60)"
orphan=subprocess.Popen([sys.executable,'-c',code,str(writer)],pass_fds=(writer,),stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
os.close(writer)
(state/'grandchild.pid').write_text(str(orphan.pid))
# Explicit readiness handshake: the new process group exists before root exit.
assert select.select([reader],[],[],5)[0], 'child readiness failed'
assert os.read(reader,1)==b'R'
os.close(reader)
'''
        result, receipt, state = self.run_guard(body)
        pid = int((state / 'grandchild.pid').read_text())
        try:
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertFalse(receipt['cleanup_complete'])
            self.assertEqual(receipt['status'], 'cleanup_failed')
            self.assertEqual(receipt['command_exit_code'], 0)
            self.assertEqual(receipt['cleanup_results'], dict(original_session='unverified',
                recorded_containers='removed', arbitrary_process_tree='unverified'))
            self.assertIn('owned session descendants remain', ' '.join(receipt['cleanup_errors']))
            trace = [json.loads(line) for line in (self.base / 'trace').read_text().splitlines()]
            removed = [args[-1] for args in trace if args[:2] == ['rm', '-f']]
            self.assertEqual(removed, [entry['name'] for entry in receipt['containers']])
        finally:
            self.clean_known_descendant(pid, receipt['child_pid'])


if __name__ == '__main__':
    unittest.main()
