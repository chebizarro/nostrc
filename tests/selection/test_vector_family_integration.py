"""Bounded renewal/enrollment seams; component physical proofs remain separate."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from selector_support import ROOT, change, s
from nip44_vector_projection import (CHECK, REGISTRATION, RENEWAL, SOURCES,
                                     before_nip44_vector_renewal, renewal_witness)


class VectorFamilyIntegration(unittest.TestCase):
    def setUp(self):
        from supported_profile_support import before_supported_batch_policy
        self.current_policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.policy = before_supported_batch_policy(self.current_policy)
        self.witness = renewal_witness()
        self.contracts = self.policy['diagnostic_nonmember_subsets']

    def test_exact_two_values_and_single_selected_registration_reverse(self):
        old = json.loads(s.git(ROOT, 'show', self.witness['previous_inventory_source'] +
                               ':tests/selection/inventory.json').stdout)
        for profile, contract in self.contracts.items():
            with self.subTest(profile=profile):
                snapshot = copy.deepcopy(contract)
                previous = before_nip44_vector_renewal(contract, profile)
                self.assertEqual(contract, snapshot)
                self.assertEqual(previous, old['diagnostic_nonmember_subsets'][profile])
                before, after = (c['review']['source_inputs'] for c in (previous, contract))
                self.assertEqual(before.keys(), after.keys())
                self.assertEqual(len(after), 193)
                self.assertEqual({p for p in before if before[p] != after[p]}, SOURCES)
                current = self.current_policy['diagnostic_nonmember_subsets'][profile]['review']['source_inputs']
                self.assertEqual({path for path in after if after[path] != current[path]},
                                 {'gnome/groundhog/TestInventory.cmake'})
                for path, expected in current.items():
                    self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected)
                for key in contract.keys() - {'review', 'primary_registrations', 'required_closure'}:
                    self.assertEqual(contract[key], previous[key], key)
                for key in contract['review'].keys() - {'source_inputs'}:
                    self.assertEqual(contract['review'][key], previous['review'][key], key)
                for key in ('primary_registrations', 'required_closure'):
                    self.assertEqual(contract[key], sorted(previous[key] +
                        ([REGISTRATION] if profile == 'grotto.linux' else [])))
                self.assertEqual(len(contract['registrations']), 33)

    def test_old_source_missing_control_and_closure_mutations_refuse(self):
        for profile, contract in self.contracts.items():
            for mutation in ('old_source', 'missing_source', 'extra_source', 'unrelated_source',
                             'omission', 'profile', 'extra_primary', 'missing_fixture'):
                with self.subTest(profile=profile, mutation=mutation):
                    bad = copy.deepcopy(contract)
                    inputs = bad['review']['source_inputs']
                    path = sorted(SOURCES)[0]
                    if mutation == 'old_source':
                        inputs[path] = self.witness['changed_source_inputs'][path]['before']
                    elif mutation == 'missing_source':
                        del inputs[path]
                    elif mutation == 'extra_source':
                        inputs['unreviewed.cmake'] = '0' * 64
                    elif mutation == 'unrelated_source':
                        inputs[next(p for p in inputs if p not in SOURCES)] = '0' * 64
                    elif mutation == 'omission':
                        bad['registrations'].pop()
                    elif mutation == 'profile':
                        bad['profile_hash'] = '0' * 64
                    elif mutation == 'extra_primary':
                        bad['primary_registrations'].append('test_interop')
                    else:
                        bad['required_closure'].pop()
                    with self.assertRaises(s.ContractError):
                        before_nip44_vector_renewal(bad, profile)

    def test_single_grotto_check_uses_descriptor_not_duplicate_runtime_authority(self):
        check = self.policy['checks'][CHECK]
        self.assertEqual(check['profiles'], ['grotto.linux'])
        self.assertEqual(check['ctest_tests'], {'grotto.linux': [REGISTRATION]})
        self.assertEqual(check['build_targets'], [REGISTRATION])
        self.assertEqual(check['runtime_inputs'], [])
        self.assertEqual(check['nodes'], ['execution.grotto', 'nip44', 'nip44.test_inputs'])
        for profile in self.contracts:
            names = [n for c in self.policy['checks'].values()
                     for n in c.get('ctest_tests', {}).get(profile, [])]
            self.assertEqual(names.count(REGISTRATION), int(profile == 'grotto.linux'))
            self.assertNotIn('test_interop', names)
        plan = s.make_plan(self.policy, [change(self.witness['canonical_input']['path'])],
                           head='a' * 40, base='b' * 40)
        self.assertIn(CHECK, plan['selected_checks'])

    def test_official_json_and_physical_case_witness_are_pinned(self):
        canonical = self.witness['canonical_input']
        raw = (ROOT / canonical['path']).read_bytes()
        self.assertEqual(len(raw), canonical['bytes'])
        self.assertEqual(hashlib.sha256(raw).hexdigest(), canonical['sha256'])
        path = ROOT / 'tests/selection/fixtures/nip44_canonical_vectors_20261022.json'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         self.witness['physical_proof']['canonical_witness_sha256'])
        self.assertEqual(sum(json.loads(path.read_bytes())['executed_cases'].values()), 128)
        changed = self.witness['changed_test_source_input']
        self.assertEqual(changed['path'], 'nips/nip44/tests/test_nip44_vectors.c')
        old = s.git(ROOT, 'show', self.witness['base'] + ':' + changed['path']).stdout
        self.assertEqual(hashlib.sha256(old).hexdigest(), changed['before'])
        self.assertEqual(hashlib.sha256((ROOT / changed['path']).read_bytes()).hexdigest(), changed['after'])

    def test_all_base_history_and_original_sanitizer_authority_remain_exact(self):
        paths = s.git(ROOT, 'ls-tree', '-r', '--name-only', self.witness['base'], '--',
                      'tests/selection/fixtures', 'tests/selection/golden').stdout.decode().splitlines()
        hashes = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths}
        self.assertEqual(len(hashes), self.witness['historical_file_count'])
        self.assertEqual(s.digest(hashes), self.witness['historical_hash_map_digest'])
        self.assertEqual(s.digest(self.policy['diagnostic_nonmembers']),
                         self.witness['sanitizer_authority_hash'])
        self.assertFalse(self.policy['enforcement_ready'])

    def test_witness_byte_changes_refuse_before_projection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / RENEWAL
            target.parent.mkdir(parents=True)
            target.write_bytes((ROOT / RENEWAL).read_bytes() + b'\n')
            with mock.patch('nip44_vector_projection.ROOT', root):
                with self.assertRaisesRegex(s.ContractError, 'witness bytes changed'):
                    before_nip44_vector_renewal(self.contracts['grotto.linux'], 'grotto.linux')


if __name__ == '__main__':
    unittest.main()
