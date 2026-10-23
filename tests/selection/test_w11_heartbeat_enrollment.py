"""Exact heartbeat input enrollment, not a scheduling remedy or hosted proof."""
import copy
import hashlib
import unittest

import closeout_support as c
from profile_contract_support import source_job_blocks
from selector_support import ROOT, s


class HeartbeatEnrollmentTests(unittest.TestCase):
    def test_actual_approved_sources_and_workflow_are_bound(self):
        policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        witness = c.heartbeat_witness()
        observed = source_job_blocks(ROOT)
        for path, expected in witness['source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected, path)
            self.assertIn('{source}/' + path, policy['checks']['selection.contract']['runtime_inputs'])
            self.assertIn('selection.contract', s.classify(policy, path)['nodes'])
            if path.startswith('nostr-gobject/'):
                self.assertIn('nostr.gobject', s.classify(policy, path)['nodes'])
        for ident, record in witness['workflow_jobs'].items():
            self.assertEqual(observed[ident], record['after'])
            self.assertEqual({field: policy['workflow_jobs'][ident][field]
                              for field in record['after']}, record['after'])
        changed = copy.deepcopy(policy['workflow_jobs'])
        ident = next(iter(witness['workflow_jobs']))
        changed[ident]['workflow_sha256'] = '0' * 64
        with self.assertRaisesRegex(s.ContractError, 'heartbeat workflow'):
            c.before_heartbeat_jobs(changed)

    def test_probe_is_separate_and_record_only(self):
        workflow = (ROOT / '.github/workflows/nostrc-ci.yml').read_text()
        test = workflow.index('      - name: Test\n')
        probe = workflow.index('      - name: Heartbeat scheduling probe')
        self.assertLess(test, probe)
        block = workflow[probe:workflow.index('      - name: Short fuzz', probe)]
        self.assertIn('continue-on-error: true', block)
        self.assertIn("!cancelled() && startsWith(matrix.os, 'macos')", block)
        self.assertNotIn('NOSTRC_HEARTBEAT_CONTROL', workflow[test:probe].split('\n      # Record-only')[0])
        script = (ROOT / 'nostr-gobject/tests/heartbeat_sched/heartbeat-sched-probe.sh').read_text()
        self.assertIn('run_one inherit inherit "$i" 0', script)
        self.assertIn('run_one fg fg "$i" 0', script)
        self.assertNotIn('kill ', script)
        self.assertTrue(script.rstrip().endswith('exit 0'))


if __name__ == '__main__':
    unittest.main()
