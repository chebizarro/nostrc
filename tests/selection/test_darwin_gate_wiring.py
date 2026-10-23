"""Public test-gate CLI wiring for explicit Darwin custody: real runs, no fakes.

Diagnostic only (never .23/profile acceptance). The adapter must hold the
helper-issued tag from a pre-gate handshake before any target can run; a helper
without that authority interface (or any invalid handshake) refuses before
launch. End-to-end command/CTest/cancel/death proofs need that helper interface.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / 'scripts/test-gate.sh'


def gate(*argv, timeout=120):
    return subprocess.run(['bash', str(GATE), '--repo', str(ROOT), *argv], capture_output=True, timeout=timeout,
                          env=dict(os.environ, XPC_SERVICE_NAME='0'))


class DarwinGateWiring(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='nostrc-darwin-gate-')
        self.addCleanup(tmp.cleanup)
        self.evidence = Path(tmp.name) / 'evidence'

    def receipt(self):
        return json.loads(next(self.evidence.glob('*/adapter-receipt.json')).read_text())

    def test_darwin_value_is_explicit_and_legacy_stays_default(self):
        result = gate('--head', 'HEAD', '--explain', '--evidence-dir', str(self.evidence))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.receipt()['custody_mode'], 'legacy')
        result = gate('--head', 'HEAD', '--native-custody', 'darwin', '--namespace-apparmor-profile',
                      'nostrc-ns-' + '0' * 32, '--evidence-dir', str(self.evidence / 'x'))
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'requires --native-custody pidns', result.stderr)

    if sys.platform == 'darwin':
        def test_pidns_still_refuses_on_darwin_before_checkout(self):
            result = gate('--head', 'HEAD', '--native-custody', 'pidns', '--evidence-dir', str(self.evidence))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b'native kernel custody unavailable on macOS', result.stderr)
            self.assertFalse(any(self.evidence.glob('*/0/source')))

        def test_darwin_refuses_before_launch_without_helper_tag_authority(self):
            # With a helper lacking the --tag-fd handshake (or any invalid tag),
            # the gate is never released: no checkout/target ever runs.
            result = gate('--head', 'HEAD', '--native-custody', 'darwin', '--evidence-dir', str(self.evidence))
            self.assertEqual(result.returncode, 1, result.stderr)
            receipt = self.receipt()
            self.assertEqual(receipt['status'], 'failed')
            self.assertEqual(receipt['custody_mode'], 'darwin')
            self.assertFalse(receipt['complete'])
            self.assertFalse(receipt['complete_gate_eligible'])
            self.assertFalse(receipt['untraced_profile_parity'])
            record, = receipt['darwin_custody']
            self.assertEqual(record['status'], 'refused_before_launch')
            self.assertNotIn('tag_sha256', record)
            self.assertEqual(record['helper_exit'], 125)
            evidence = json.loads(Path(record['evidence']).read_text())
            self.assertFalse(evidence['launched'])
            self.assertFalse(evidence['cleanup_complete'])
            self.assertIn('error', evidence)
            self.assertNotIn('recovery_exit', record)  # nothing launched: nothing to recover
            self.assertFalse(any(self.evidence.glob('*/0/source/.git')))
            self.assertIn(b'tag handshake', result.stderr)
    else:
        def test_darwin_refuses_on_linux_before_launch(self):
            result = gate('--head', 'HEAD', '--native-custody', 'darwin', '--evidence-dir', str(self.evidence))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn(b'explicit macOS-only diagnostic', result.stderr)
            self.assertFalse(any(self.evidence.glob('*/0/source/.git')))


if __name__ == '__main__':
    unittest.main()
