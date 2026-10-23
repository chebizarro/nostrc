"""Physical retained-plan execution through the existing public outer adapter."""
import json
import os
from pathlib import Path
import subprocess
import unittest

import test_local_command_adapter as fixture
from selector_support import s


class RetainedPlanAdapterTests(unittest.TestCase):
    setUp = fixture.LocalCommandAdapterTests.setUp
    write = fixture.LocalCommandAdapterTests.write
    git = fixture.LocalCommandAdapterTests.git
    save_policy = fixture.LocalCommandAdapterTests.save_policy
    commit = fixture.LocalCommandAdapterTests.commit

    def plan(self):
        return s.git_plan(self.repo, self.policy, head=self.head, base=self.head,
                          mode='broad', context='ci', event={'kind': 'manual'})

    def run_plan(self, plan, *, success=True, extra=()):
        path = self.work / 'original-plan.json'
        path.write_text(json.dumps(plan))
        evidence = self.work / ('evidence-' + str(len(list(self.work.glob('evidence-*')))))
        result = subprocess.run(['bash', str(self.repo / 'scripts/test-gate.sh'), '--repo', str(self.repo),
            '--head', self.head, '--plan', str(path), '--profile', 'local.command',
            '--evidence-dir', str(evidence), *extra], capture_output=True, text=True,
            env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'), timeout=60)
        receipt_path = next(evidence.glob('*/adapter-receipt.json'))
        receipt = json.loads(receipt_path.read_text())
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr + str(receipt))
        self.assertEqual(json.loads(path.read_text()), plan)
        return receipt, receipt_path.parent

    def test_actual_command_retains_original_plan_and_incomplete_scope(self):
        plan = self.plan()
        receipt, directory = self.run_plan(plan)
        self.assertEqual(json.loads((directory / '0/plan.json').read_text()), plan)
        self.assertFalse(receipt['complete'])
        self.assertFalse(receipt['complete_gate_eligible'])
        execution = receipt['executions'][0]
        self.assertEqual(execution['scope'], 'explicit_retained_plan_lane_projection')
        lane = execution['lanes'][0]
        raw = json.loads((Path(lane['evidence']) / 'receipt.json').read_text())
        self.assertEqual(raw['plan_id'], plan['plan_id'])
        self.assertEqual(raw['source_identity'], plan['source_identity'])
        self.assertEqual(raw['backend_receipt']['outcome'], 'passed')

    def test_mutated_or_wrong_source_plan_refuses_before_command(self):
        for case in ('id', 'source'):
            with self.subTest(case=case):
                plan = self.plan()
                if case == 'id':
                    plan['plan_id'] = '0' * 64
                else:
                    plan['source_identity']['sha'] = '0' * 40
                receipt, directory = self.run_plan(plan, success=False)
                self.assertEqual(receipt['executions'][0]['lanes'], [])
                self.assertFalse(list(directory.rglob('proof.txt')))

    def test_retained_plan_refuses_planner_override(self):
        receipt, _ = self.run_plan(self.plan(), success=False, extra=('--broad',))
        self.assertIn('--plan requires --head', receipt['errors'][0])


if __name__ == '__main__':
    unittest.main()
