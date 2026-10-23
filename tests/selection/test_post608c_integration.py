"""Finite integration provenance, not hosted execution/profile acceptance."""
import copy
import hashlib
import unittest
from selector_support import ROOT, s
import post608c_integration_support as wave


class Post608cIntegration(unittest.TestCase):
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_exact_policy_inverse_without_omission_witness_changes(self):
        current = self.policy()
        before = wave.before_policy(current)
        # The fixed witness digest avoids depending on historical Git objects in
        # shallow hosted checkouts; it was captured from exact 608c inventory.
        self.assertEqual(s.digest(before), wave.witness()['base_policy_hash'])
        self.assertEqual(wave.before_policy(before), before)
        self.assertNotEqual(current, before)
        for field in ('profiles', 'diagnostic_nonmembers', 'diagnostic_nonmember_subsets'):
            self.assertEqual(current[field], before[field])
        self.assertFalse(current['enforcement_ready'])

    def test_exact_source_inverse_and_drift_refusal(self):
        for path, record in wave.witness()['sources'].items():
            with self.subTest(path=path):
                raw = (ROOT / path).read_bytes()
                restored = wave.before_source(path, raw)
                self.assertEqual(hashlib.sha256(restored).hexdigest(), record['before_sha256'])
                self.assertEqual(wave.before_source(path, restored), restored)
                with self.assertRaises(ValueError):
                    wave.before_source(path, raw + b'\n# unreviewed edit\n')

    def test_partial_or_mutated_enrollment_refuses(self):
        data = wave.witness()
        for field, records in data['maps'].items():
            for ident in records:
                bad = self.policy()
                bad[field][ident]['unreviewed'] = True
                with self.subTest(field=field, ident=ident), self.assertRaises(ValueError):
                    wave.before_policy(bad)
        for path in data['runtime_inputs']:
            for operation in ('remove', 'duplicate'):
                bad = self.policy()
                inputs = bad['checks']['selection.contract']['runtime_inputs']
                inputs.remove(path) if operation == 'remove' else inputs.append(path)
                with self.subTest(path=path, operation=operation), self.assertRaises(ValueError):
                    wave.before_policy(bad)
        for field, key in (('path_rules', 'added_path_rules'), ('coverage_gaps', 'added_coverage_gaps')):
            bad = self.policy(); bad[field].remove(data[key][0])
            with self.subTest(field=field), self.assertRaises(ValueError): wave.before_policy(bad)
        bad = self.policy()
        bad['workflow_jobs'] = copy.deepcopy(wave.before_policy(bad)['workflow_jobs'])
        with self.assertRaises(ValueError): wave.before_policy(bad)

    def test_actual_workflows_enrolled_and_unknown_jobs_not_projected(self):
        from profile_contract_support import source_job_blocks
        actual = source_job_blocks(ROOT)
        policy = self.policy()
        for ident, record in wave.witness()['maps']['workflow_jobs'].items():
            for key in ('source_sha256', 'workflow_sha256', 'matrix_source'):
                self.assertEqual(policy['workflow_jobs'][ident][key], actual[ident][key])
            self.assertEqual(wave.before_jobs(actual)[ident],
                             {key: record['before'][key] for key in actual[ident]})
        ident = '.github/workflows/dependency-worker.yml:execute'
        bad = copy.deepcopy(actual); bad[ident]['source_sha256'] = '0' * 64
        self.assertEqual(wave.before_jobs(bad)[ident], bad[ident])
