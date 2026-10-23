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


class HostedCauseIntegration(unittest.TestCase):
    """Exact authored hosted repair enrollment; no Docker/runtime acceptance."""
    def policy(self):
        return s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_exact_9062_policy_inverse_and_old_witnesses_stay_immutable(self):
        current = self.policy()
        before = wave.before_hosted_policy(current)
        self.assertEqual(s.digest(before), wave.hosted_witness()['base_policy_hash'])
        self.assertEqual(wave.before_hosted_policy(before), before)
        for field in ('profiles', 'nodes', 'coverage_gaps', 'diagnostic_nonmembers',
                      'diagnostic_nonmember_subsets'):
            self.assertEqual(current[field], before[field])
        self.assertEqual(s.digest(wave.before_policy(current)), wave.witness()['base_policy_hash'])
        self.assertFalse(current['enforcement_ready'])

    def test_exact_hosted_sources_inverse_and_unknown_bytes_refuse(self):
        for path, record in wave.hosted_witness()['sources'].items():
            with self.subTest(path=path):
                raw = (ROOT / path).read_bytes()
                before = wave.before_hosted_source(path, raw)
                self.assertEqual(hashlib.sha256(before).hexdigest(), record['before_sha256'])
                self.assertEqual(wave.before_hosted_source(path, before), before)
                with self.assertRaises(ValueError):
                    wave.before_hosted_source(path, raw + b'\n# unknown edit\n')

    def test_missing_duplicate_or_mixed_enrollment_refuses(self):
        data = wave.hosted_witness()
        for value in data['runtime_inputs']:
            for duplicate in (False, True):
                bad = self.policy(); inputs = bad['checks']['selection.contract']['runtime_inputs']
                inputs.append(value) if duplicate else inputs.remove(value)
                with self.subTest(value=value, duplicate=duplicate), self.assertRaises(ValueError):
                    wave.before_hosted_policy(bad)
        for ident in data['maps']['workflow_jobs']:
            bad = self.policy(); bad['workflow_jobs'][ident]['source_sha256'] = '0' * 64
            with self.subTest(ident=ident), self.assertRaises(ValueError): wave.before_hosted_policy(bad)
        bad = self.policy(); bad['workflow_jobs'] = wave.before_hosted_jobs(bad['workflow_jobs'])
        with self.assertRaises(ValueError): wave.before_hosted_policy(bad)

    def test_controller_owner_inputs_and_exact_workflow_hashes(self):
        from profile_contract_support import source_job_blocks, validate_job_accounting
        current = self.policy(); validate_job_accounting(current, ROOT)
        actual = source_job_blocks(ROOT)
        for ident, record in wave.hosted_witness()['maps']['workflow_jobs'].items():
            for key in ('source_sha256', 'workflow_sha256', 'matrix_source'):
                self.assertEqual(current['workflow_jobs'][ident][key], actual[ident][key])
            self.assertEqual(wave.before_hosted_jobs(actual)[ident],
                             {key: record['before'][key] for key in actual[ident]})
        controller = 'tests/selection/namespace_policy_controller.sh'
        classified = s.classify(current, controller)
        self.assertEqual(classified['effect'], 'global')
        self.assertIn('selection.contract', classified['nodes'])
        self.assertIn('{source}/' + controller, current['checks']['selection.contract']['runtime_inputs'])
        self.assertTrue((ROOT / controller).is_file())
        self.assertIn('tests/selection', current['nodes']['selection.contract']['owner_paths'])

    def test_full_gate_fixture_owns_normal_validator_import_and_missing_copy_refuses(self):
        import re
        import shutil
        import subprocess
        import sys
        import tempfile
        from pathlib import Path
        text = (ROOT / 'scripts/test-full-gate.sh').read_text()
        modules = re.findall(r'cp "\$scripts/(test_selection(?:_[a-z0-9_]+)?\.py)" "\$repo/scripts/\1"', text)
        self.assertIn('test_selection_execution.py', modules)
        with tempfile.TemporaryDirectory(prefix='nostrc-fullgate-import-') as directory:
            scripts = ROOT / 'scripts'; fixture = Path(directory)
            for module in modules:
                shutil.copy2(scripts / module, fixture / module)
            argv = [sys.executable, '-B', '-I', '-c',
                    'import sys;sys.path.insert(0,sys.argv[1]);import test_selection as s;s.load_inventory(sys.argv[2])',
                    str(fixture), str(ROOT / 'tests/selection/inventory.json')]
            positive = subprocess.run(argv, capture_output=True, timeout=30)
            self.assertEqual(positive.returncode, 0, positive.stderr)
            (fixture / 'test_selection_execution.py').unlink()
            negative = subprocess.run(argv, capture_output=True, timeout=30)
            self.assertNotEqual(negative.returncode, 0)
            self.assertIn(b"ModuleNotFoundError: No module named 'test_selection_execution'", negative.stderr)
