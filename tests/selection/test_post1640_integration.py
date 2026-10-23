"""Finite A/C source enrollment, not Linux execution or W11 acceptance."""
import copy
import hashlib
import unittest
from selector_support import ROOT, s
import post1640_integration_support as wave


class Post1640Integration(unittest.TestCase):
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_exact_1640_inverse_and_composed_historical_projections(self):
        policy = self.policy()
        before = wave.before_policy(policy)
        self.assertEqual(s.digest(before), wave.witness()['base_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        for field in ('diagnostic_nonmembers', 'diagnostic_nonmember_subsets', 'coverage_gaps'):
            self.assertEqual(policy[field], before[field])
        import post608c_integration_support as older
        self.assertEqual(s.digest(older.before_hosted_policy(policy)), older.hosted_witness()['base_policy_hash'])
        self.assertEqual(s.digest(older.before_policy(policy)), older.witness()['base_policy_hash'])
        self.assertFalse(policy['enforcement_ready'])

    def test_unrelated_libgo_mutation_survives_and_original_subset_refusal_applies(self):
        import test_selection_subset as subset
        from runtime_integration_support import before_runtime_policy
        policy = self.policy()
        check = policy['checks']['supported.libgo']
        check.pop(subset.FULL_FIELD)
        for ident in subset.PROFILES:
            check['profiles'].remove(ident)
            del check['ctest_tests'][ident]
        before = wave.before_policy(policy)
        self.assertEqual(before['checks']['supported.libgo'], check)
        self.assertEqual(wave.before_policy(before), before)
        with self.assertRaisesRegex(ValueError, 'stale profile hash'):
            subset.active_contracts(policy)
        historical = before_runtime_policy(policy)
        self.assertEqual(set(subset.active_contracts(historical)), set(subset.PROFILES))

    def test_unrelated_node_mutation_reaches_original_closeout_refusal(self):
        from closeout_support import before_closeout_policy
        policy = self.policy()
        policy['nodes']['groundhog.gresource_inputs']['owner_paths'].append('unrelated/input.h')
        before = wave.before_policy(policy)
        self.assertEqual(before['nodes'], policy['nodes'])
        self.assertEqual(wave.before_policy(before), before)
        with self.assertRaisesRegex(ValueError, 'closeout nodes changed'):
            before_closeout_policy(policy)

    def test_exact_seven_author_markers_not_homed_or_other_profiles(self):
        policy = self.policy()
        observed = {ident for ident, profile in policy['profiles'].items()
                    if 'normal_selected_execution' in profile}
        self.assertEqual(observed, set(wave.witness()['selected_profiles']))
        before = wave.before_policy(policy)
        for ident in observed:
            authored = copy.deepcopy(policy['profiles'][ident])
            del authored['normal_selected_execution']
            self.assertEqual(authored, before['profiles'][ident])
        self.assertEqual(policy['profiles']['homed.linux.units'], before['profiles']['homed.linux.units'])
        self.assertNotIn('normal_selected_execution', policy['profiles']['homed.linux.ci'])
        self.assertEqual(policy['profiles']['homed.linux.ci']['contexts'], ['ci'])
        self.assertNotIn('homed.linux.ci', before['profiles'])

    def test_exact_source_inverse_idempotence_and_unreviewed_byte_refusal(self):
        for path, record in wave.witness()['sources'].items():
            with self.subTest(path=path):
                raw = (ROOT / path).read_bytes()
                before = wave.before_source(path, raw)
                self.assertEqual(hashlib.sha256(before).hexdigest(), record['before_sha256'])
                self.assertEqual(wave.before_source(path, before), before)
                with self.assertRaises(ValueError):
                    wave.before_source(path, raw + b'\n# unreviewed source\n')

    def test_missing_duplicate_mutated_and_mixed_enrollment_refuse(self):
        data = wave.witness()
        for path in data['runtime_inputs']:
            for duplicate in (False, True):
                policy = self.policy()
                inputs = policy['checks']['selection.contract']['runtime_inputs']
                inputs.append(path) if duplicate else inputs.remove(path)
                with self.subTest(path=path, duplicate=duplicate), self.assertRaises(ValueError):
                    wave.before_policy(policy)
        for field, records in data['maps'].items():
            for ident in records:
                policy = self.policy()
                policy[field][ident]['unreviewed'] = True
                with self.subTest(field=field, ident=ident), self.assertRaises(ValueError):
                    wave.before_policy(policy)
        policy = self.policy()
        policy['workflow_jobs'] = wave.before_policy(policy)['workflow_jobs']
        with self.assertRaises(ValueError):
            wave.before_policy(policy)
        policy = self.policy()
        policy['checks']['selection.contract']['runtime_inputs'] = [
            path for path in policy['checks']['selection.contract']['runtime_inputs']
            if path not in data['runtime_inputs']]
        with self.assertRaises(ValueError):
            wave.before_policy(policy)

    def test_actual_workflows_and_only_exact_new_homed_job_project(self):
        from profile_contract_support import source_job_blocks, validate_job_accounting
        policy = self.policy()
        validate_job_accounting(policy, ROOT)
        actual = source_job_blocks(ROOT)
        before = wave.before_jobs(actual)
        for ident, record in wave.witness()['maps']['workflow_jobs'].items():
            for field in ('source_sha256', 'workflow_sha256', 'matrix_source'):
                self.assertEqual(policy['workflow_jobs'][ident][field], actual[ident][field])
            if record['before'] is None:
                self.assertNotIn(ident, before)
            else:
                self.assertEqual(before[ident], {key: record['before'][key] for key in actual[ident]})
        ident = '.github/workflows/dependency-tests.yml:homed_units'
        bad = copy.deepcopy(actual)
        bad[ident]['source_sha256'] = '0' * 64
        self.assertEqual(wave.before_jobs(bad)[ident], bad[ident])
        bad = copy.deepcopy(actual)
        bad[ident]['unreviewed'] = True
        self.assertEqual(wave.before_jobs(bad)[ident], bad[ident])
        bad = copy.deepcopy(actual)
        bad['unknown:job'] = copy.deepcopy(actual[ident])
        self.assertEqual(wave.before_jobs(bad)['unknown:job'], bad['unknown:job'])

    def test_original_witnesses_and_author_production_bytes_unchanged(self):
        data = wave.witness()
        for field in ('protected_witnesses', 'author_sources'):
            for path, digest in data[field].items():
                with self.subTest(field=field, path=path):
                    self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest)

    def test_new_runtime_inputs_have_live_command_ownership(self):
        policy = self.policy()
        inputs = policy['checks']['selection.contract']['runtime_inputs']
        for path in wave.witness()['runtime_inputs']:
            with self.subTest(path=path):
                self.assertEqual(inputs.count(path), 1)
                relative = path.removeprefix('{source}/')
                self.assertTrue((ROOT / relative).is_file())
                classified = s.classify(policy, relative)
                self.assertEqual(classified['effect'], 'global')
                self.assertIn('selection.contract', classified['nodes'])
