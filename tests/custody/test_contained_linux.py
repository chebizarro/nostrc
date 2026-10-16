"""Explicit real Docker gate, not simulated acceptance.

NOSTRC_CUSTODY_IMAGE=sha256:... python3 -m unittest discover -s tests/custody -v
Unavailable Docker/image is a failure, not a passing or skipped custody gate.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from test_selection_custody import ContainerOwner


class ActualContainedLinux(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = os.environ['NOSTRC_CUSTODY_IMAGE']
        subprocess.run(['docker', 'info'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='nostrc-contained-test-'))
        print('retained custody evidence: ' + str(self.root), flush=True)

    def owner(self, name, grace=0.25):
        work = self.root / (name + '-work')
        work.mkdir()
        return ContainerOwner(self.root / name, grace=grace, rpc_timeout=30), work

    def invoke(self, owner, work, mode):
        return owner.run(self.image, ['python3', '/probe/workload.py', 'wrapped', mode],
            mounts=[(ROOT / 'tests/custody', '/probe', True), (work, '/work', False)], timeout=45,
            binding={'adversary': mode, 'source_file': str(ROOT / 'tests/custody/workload.py')})

    def assert_final(self, owner):
        receipt = owner.receipt
        self.assertTrue(receipt['cleanup_complete'], receipt)
        self.assertFalse(receipt['complete_gate_eligible'])
        self.assertFalse(receipt['complete'])
        self.assertEqual(receipt['cleanup_inspect']['State']['Pid'], 0)
        self.assertFalse(receipt['cleanup_inspect']['State']['Running'])
        self.assertEqual(receipt['container_id'], receipt['removed_container_id'])
        self.assertEqual(subprocess.check_output(['docker', 'ps', '-aq', '--no-trunc', '--filter', 'id=' + owner.cid]).strip(), b'')
        self.assertIn(b'owned ready \xff\n', (owner.directory / 'attach.raw').read_bytes())

    def test_real_wrapped_configure_build_ctest_success_and_detached_cleanup(self):
        owner, work = self.owner('success')
        self.assertEqual(self.invoke(owner, work, 'success'), 0, str(owner.directory))
        self.assertTrue((work / 'built').is_file())
        actor = json.loads((work / 'escape.json').read_text())
        self.assertEqual(actor['pid'], actor['pgid'])
        self.assertEqual(actor['pid'], actor['sid'])
        self.assert_final(owner)

    def test_real_wrapped_generic_command_and_detached_cleanup(self):
        owner, work = self.owner('command')
        self.assertEqual(self.invoke(owner, work, 'command'), 0, str(owner.directory))
        self.assertFalse((work / 'built').exists())
        self.assert_final(owner)

    def test_real_wrapped_failure_retains_raw_bytes_and_negative_result(self):
        owner, work = self.owner('failure')
        self.assertNotEqual(self.invoke(owner, work, 'fail'), 0)
        self.assertEqual(owner.receipt['status'], 'failed')
        self.assertTrue((work / 'results.xml').is_file())
        self.assert_final(owner)

    def test_real_cancellation_escaped_term_ignorer_and_unrelated_sibling(self):
        sibling, sibling_work = self.owner('sibling')
        worker, work = self.owner('cancel')
        sibling_result, result, errors = [], [], []
        def run(target, output, call):
            try:
                output.append(call())
            except BaseException as exc:
                errors.append(exc)
        sibling_thread = threading.Thread(target=run, args=(sibling, sibling_result,
            lambda: sibling.run(self.image, ['python3', '-c', 'import signal; signal.pause()'], workdir='/tmp', timeout=90)))
        thread = threading.Thread(target=run, args=(worker, result, lambda: self.invoke(worker, work, 'hold')))
        sibling_thread.start()
        thread.start()
        reader = None
        try:
            self.assertTrue(sibling.running.wait(30))
            self.assertTrue(worker.running.wait(30))
            reader = subprocess.Popen(['docker', 'logs', '--follow', worker.cid], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            for line in iter(reader.stdout.readline, b''):
                if b'owned ready \xff\n' in line:
                    break
            else:
                self.fail('actual consumer never became ready')
            worker.cancel.requested.set()
            worker.cancel.wakeup.set()
            thread.join(45)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(result, [130])
            self.assertEqual(worker.receipt['status'], 'cancelled')
            self.assert_final(worker)
            self.assertEqual(subprocess.check_output(['docker', 'exec', sibling.cid, 'python3', '-c', "print('sibling alive')"]).strip(), b'sibling alive')
        finally:
            worker.cancel.requested.set()
            worker.cancel.wakeup.set()
            sibling.cancel.requested.set()
            sibling.cancel.wakeup.set()
            thread.join(45)
            sibling_thread.join(45)
            if reader:
                reader.terminate()
                reader.wait(timeout=10)
                reader.stdout.close()
        self.assertFalse(sibling_thread.is_alive())
        self.assertTrue(sibling.receipt['cleanup_complete'], sibling.receipt)


if __name__ == '__main__':
    unittest.main()
