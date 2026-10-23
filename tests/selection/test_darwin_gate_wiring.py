"""test-gate explicit Darwin custody wiring: real native runs of C's helper, no fakes.

Diagnostic only (never .23/profile acceptance). Success and recovery authority
come only from the helper's private control pipe; recovery uses the
adapter-held original tag names and never makes a lane pass.
"""
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / 'scripts/test-gate.sh'
HELPER = ROOT / 'scripts/adapter_darwin_custody.py'
sys.path.insert(0, str(ROOT / 'scripts'))
NAME = re.compile(r'org\.nostrc\.custody\.[0-9a-f]{32}\.(a|b)')

ESCAPE = '''
import os, signal, sys, time
from pathlib import Path
marker = Path(sys.argv[1])
if os.fork() == 0:
    os.setsid()
    if os.fork() == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        marker.write_text(str(os.getpid()))
        time.sleep(120)
        os._exit(0)
    os._exit(0)
os.wait()
while not marker.exists():
    time.sleep(0.01)
Path(str(marker) + '.supervisor').write_text(str(os.getppid()))
time.sleep(float(sys.argv[2]))
'''


def alive(pid):
    try:
        os.kill(pid, 0)  # observation only; never a cleanup signal
        return True
    except ProcessLookupError:
        return False


def wait_for(path, timeout=30):
    deadline = time.monotonic() + timeout
    while not path.exists():
        if time.monotonic() > deadline:
            raise AssertionError('missing readiness file ' + str(path))
        time.sleep(0.02)


def gate(*argv, timeout=180):
    return subprocess.run(['bash', str(GATE), '--repo', str(ROOT), *argv], capture_output=True, timeout=timeout,
                          env=dict(os.environ, XPC_SERVICE_NAME='0'))


class DarwinGateWiring(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='nostrc-darwin-gate-')
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.evidence = self.work / 'evidence'

    def receipt(self):
        return json.loads(next(self.evidence.glob('*/adapter-receipt.json')).read_text())

    def test_darwin_value_is_explicit_and_legacy_stays_default(self):
        result = gate('--head', 'HEAD', '--explain', '--evidence-dir', str(self.evidence))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.receipt()['custody_mode'], 'legacy')
        self.assertNotIn('darwin_custody', self.receipt())
        result = gate('--head', 'HEAD', '--native-custody', 'darwin', '--namespace-apparmor-profile',
                      'nostrc-ns-' + '0' * 32, '--evidence-dir', str(self.work / 'x'))
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'requires --native-custody pidns', result.stderr)

    if sys.platform == 'darwin':
        def custody(self, argv, name='cmd', native_lane=None):
            from test_gate_darwin import darwin_command
            records, errors = [], []
            log = self.work / (name + '.log')
            try:
                darwin_command(argv, helper=HELPER, records=records, errors=errors, log=log, native_lane=native_lane)
                outcome = None
            except (RuntimeError, KeyboardInterrupt) as exc:
                outcome = exc
            record, = records
            self.assertEqual(json.loads(Path(str(log) + '.darwin-adapter.json').read_text()), record)
            self.assertFalse(record['complete'])
            self.assertFalse(record['complete_gate_eligible'])
            return record, outcome, log

        def assert_authoritative(self, record, exit_code):
            tag, final = record['control_tag'], record['control_final']
            self.assertTrue(NAME.fullmatch(tag['denied']) and NAME.fullmatch(tag['allowed']))
            self.assertNotEqual(tag['denied'][:-2], tag['allowed'][:-2])
            self.assertEqual((final['tag_authority'], final['launched'], final['cleanup_complete'], final['exit_code']),
                             ('supervisor', True, True, exit_code))
            self.assertEqual(record['helper_exit'], exit_code)

        def test_pidns_still_refuses_on_darwin_before_checkout(self):
            result = gate('--head', 'HEAD', '--native-custody', 'pidns', '--evidence-dir', str(self.evidence))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b'native kernel custody unavailable on macOS', result.stderr)
            self.assertFalse(any(self.evidence.glob('*/0/source')))

        def test_public_cli_runs_real_checkout_under_custody(self):
            result = gate('--head', 'HEAD', '--native-custody', 'darwin', '--explain', '--evidence-dir', str(self.evidence))
            self.assertEqual(result.returncode, 0, result.stderr)
            receipt = self.receipt()
            self.assertEqual((receipt['custody_mode'], receipt['complete'], receipt['complete_gate_eligible'],
                              receipt['untraced_profile_parity']), ('darwin', False, False, False))
            record, = receipt['darwin_custody']
            self.assertTrue(record['log'].endswith('source-checkout.log'))
            self.assertEqual(record['status'], 'clean')
            self.assert_authoritative(record, 0)

        def test_simple_and_nonzero_commands(self):
            record, outcome, log = self.custody([sys.executable, '-c', 'print("custodied")'])
            self.assertIsNone(outcome)
            self.assertEqual(record['status'], 'clean')
            self.assert_authoritative(record, 0)
            self.assertIn(b'custodied', log.read_bytes())
            record, outcome, _ = self.custody([sys.executable, '-c', 'raise SystemExit(7)'], 'nonzero')
            self.assertIn('command exited 7', str(outcome))
            self.assertEqual(record['status'], 'clean')  # cleanup complete; command failed
            self.assert_authoritative(record, 7)

        def test_escaped_descendant_is_cleaned_and_sibling_survives(self):
            sibling = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
            self.addCleanup(lambda: (sibling.kill(), sibling.wait()))
            marker = self.work / 'escaped'
            record, outcome, _ = self.custody([sys.executable, '-c', ESCAPE, str(marker), '0'], 'escape')
            self.assertIsNone(outcome)
            self.assert_authoritative(record, 0)
            self.assertGreaterEqual(record['control_final']['killed_count'], 1)
            self.assertFalse(alive(int(marker.read_text())))
            self.assertIsNone(sibling.poll())

        def runner(self, marker, name):
            script = f'''
import json, signal, sys
sys.path.insert(0, {str(ROOT / 'scripts')!r})
from pathlib import Path
from test_gate_darwin import darwin_command
def stop(signum, frame):
    raise KeyboardInterrupt()
signal.signal(signal.SIGTERM, stop)
records = []
try:
    darwin_command([sys.executable, '-c', {ESCAPE!r}, {str(marker)!r}, '120'], helper=Path({str(HELPER)!r}),
                   records=records, errors=[], log=Path({str(self.work / name)!r}))
except BaseException as exc:
    print(type(exc).__name__, exc, flush=True)
    sys.exit(3)
'''
            return subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        def test_cancellation_tears_down_tree_and_keeps_record(self):
            marker = self.work / 'cancel-escaped'
            runner = self.runner(marker, 'cancel.log')
            wait_for(Path(str(marker) + '.supervisor'))
            escaped = int(marker.read_text())
            runner.send_signal(signal.SIGTERM)
            output, _ = runner.communicate(timeout=180)
            self.assertEqual(runner.returncode, 3, output)
            self.assertIn(b'KeyboardInterrupt', output)
            record = json.loads((self.work / 'cancel.log.darwin-adapter.json').read_text())
            self.assertEqual(record['status'], 'clean')
            self.assertTrue(record['control_final']['cancelled'])
            self.assertTrue(record['control_final']['cleanup_complete'])
            self.assertFalse(alive(escaped))

        def test_helper_death_recovers_only_with_original_tag_and_lane_stays_failed(self):
            marker = self.work / 'death-escaped'
            runner = self.runner(marker, 'death.log')
            wait_for(Path(str(marker) + '.supervisor'))
            escaped, supervisor = int(marker.read_text()), int(Path(str(marker) + '.supervisor').read_text())
            os.kill(supervisor, signal.SIGKILL)  # injected abnormal helper death (test only)
            output, _ = runner.communicate(timeout=240)
            self.assertEqual(runner.returncode, 3, output)
            self.assertIn(b'recovered_lane_failed', output)
            record = json.loads((self.work / 'death.log.darwin-adapter.json').read_text())
            self.assertEqual(record['helper_exit'], -signal.SIGKILL)
            self.assertIsNone(record['control_final'])  # no authoritative final: never inferred clean
            self.assertFalse(record['cleanup_complete'])
            self.assertEqual(record['status'], 'recovered_lane_failed')
            self.assertTrue(record['recovered'])
            self.assertEqual((record['recovery_final']['tag_authority'], record['recovery_final']['cleanup_complete']),
                             ('parent-bound', True))
            recovery_log = (self.work / 'death.log.darwin-recovery.log').read_bytes()
            self.assertNotIn(b'untrusted-file', recovery_log)
            self.assertFalse(alive(escaped))
            self.assertNotEqual(record['informational_evidence'], record['recovery_informational_evidence'])

        def test_missing_or_tampered_seal_is_retained_failure(self):
            for name, target in (('missing', 'pass'),
                                 ('tampered', "import os; os.write(int(os.environ['NOSTRC_NATIVE_SEAL_FD']), b'f' * 64)")):
                with self.subTest(seal=name):
                    lane = self.work / ('lane-' + name)
                    lane.mkdir()
                    record, outcome, _ = self.custody([sys.executable, '-c', target], 'seal-' + name, native_lane=lane)
                    self.assertEqual(record['status'], 'clean')
                    self.assertIn('native final evidence refused', str(outcome))
                    final = json.loads((lane / 'native-final.json').read_text())
                    self.assertEqual((final['status'], final['complete']), ('failed', False))
    else:
        def test_darwin_refuses_on_linux_before_launch(self):
            result = gate('--head', 'HEAD', '--native-custody', 'darwin', '--evidence-dir', str(self.evidence))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b'explicit macOS-only diagnostic', result.stderr)
            self.assertFalse(any(self.evidence.glob('*/0/source/.git')))


if __name__ == '__main__':
    unittest.main()
