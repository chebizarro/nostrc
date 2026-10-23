"""Actual miniature backend state retention; not root-profile acceptance."""
from realization_support import Project, r
from selector_support import s


class CoverageRetention(Project):
    def execute_with_gap(self, gap):
        self.inventory.update(coverage_gaps=[gap], enforcement_ready=False)
        self.plan['inventory_hash'] = s.digest(self.inventory)
        realized = r.configure_profile(self.root, self.build, 'synthetic', self.profile,
                                       self.identity, s.digest(self.inventory), inventory=self.inventory)
        resolved = r.resolve_checks(self.inventory, self.plan, realized, 'synthetic')
        pre = r.check_inputs(self.inventory, realized)
        self.assertTrue(pre['passed'], pre['gaps'])
        r.build_resolved(resolved, self.build)
        post = r.check_inputs(self.inventory, realized, after_build=True)
        self.assertTrue(post['passed'], post['gaps'])
        raw = r.run_resolved(resolved, self.build, self.evidence, input_audit=post,
                             input_audit_before_build=pre, build=False)
        self.assertEqual(raw['outcome'], 'passed')
        expected = s.coverage_state(self.inventory)
        for value in (resolved, pre, post, raw):
            for key in s.COVERAGE_STATE_FIELDS:
                self.assertEqual(value[key], expected[key], key)
        self.assertEqual(raw['input_audit_before_build'], pre)
        self.assertFalse(raw['enforcement_ready'])
        self.assertFalse(raw['rollout_readiness']['ready'])
        return raw

    def test_reviewed_admin_prerequisite_retained_without_blocking_observation(self):
        gap = dict(id='shadow_cutover', tracking='nostrc-0cfsg.5', reason='operator approval remains required',
                   classification='administrative_rollout_prerequisite', authority='reviewed_shadow_cutover_v1')
        raw = self.execute_with_gap(gap)
        self.assertTrue(raw['complete_gate_eligible'])
        self.assertEqual(raw['coverage_gaps'], [gap])
        self.assertEqual(raw['rollout_prerequisites'], [gap])
        self.assertEqual(raw['physical_coverage_gaps'], [])

    def test_unmarked_historical_prerequisite_still_blocks_complete(self):
        gap = dict(id='shadow_cutover', tracking='nostrc-0cfsg.5', reason='unmarked legacy row')
        raw = self.execute_with_gap(gap)
        self.assertFalse(raw['complete_gate_eligible'])
        self.assertEqual(raw['physical_coverage_gaps'], [gap])
        self.assertEqual(raw['rollout_prerequisites'], [])
