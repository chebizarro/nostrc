"""The rename inverse reproduces the pinned historical digests and nothing else."""
import copy
import hashlib
import unittest

from selector_support import ROOT, s
import bud02_registration_rename_support as layer

OLD, NEW = 'test-hanami-bud02-auth', 'test-bud02-auth'
PROFILE = 'groundhog.linux.sanitizers'


class Bud02RegistrationRename(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_witness_is_pinned_and_current_policy_carries_only_the_new_name(self):
        self.assertEqual(hashlib.sha256((ROOT / layer.WITNESS).read_bytes()).hexdigest(), layer.SHA256)
        self.assertEqual((layer.witness()['old'], layer.witness()['new']), (OLD, NEW))
        self.assertEqual(len(layer.witness()['paths']), 8)
        self.assertIn(NEW, self.policy['diagnostic_nonmembers'][PROFILE]['registrations'])
        self.assertNotIn(OLD, self.policy['diagnostic_nonmembers'][PROFILE]['registrations'])

    def test_projection_restores_each_pinned_historical_hash(self):
        previous = layer.before_policy(self.policy)
        for profile, record in previous['diagnostic_nonmember_subsets'].items():
            self.assertEqual(record['universe_hash'], s.digest(record['registration_universe']), profile)
            self.assertIn(OLD, record['registration_universe'])
            self.assertNotIn(NEW, record['registration_universe'])
        contract = s.diagnostic_nonmember_contracts(previous)[PROFILE]
        self.assertEqual(s.digest(contract), 'c896cf762692727997bf144a2bcb881eab50ab82964b38d7cfdf7b7bccef8aa3')
        self.assertEqual(layer.before_policy(previous), previous)

    def test_only_the_recorded_name_changes(self):
        previous = layer.before_policy(self.policy)
        for path in layer.witness()['paths']:
            holder, field = layer._locate(previous, path)
            current, _ = layer._locate(self.policy, path)
            self.assertEqual(sorted(x for x in holder[field] if x != OLD),
                             sorted(x for x in current[field] if x != NEW))

    def test_two_sided_unsorted_or_missing_lists_refuse(self):
        names = self.policy['diagnostic_nonmembers'][PROFILE]['registrations']
        with self.assertRaisesRegex(s.ContractError, 'one-sided'):
            layer.before_registrations(sorted(names + [OLD]))
        with self.assertRaisesRegex(s.ContractError, 'one-sided'):
            layer.before_registrations([n for n in names if n != NEW])
        with self.assertRaisesRegex(s.ContractError, 'sorted'):
            layer.before_registrations(list(reversed(names)))
        forged = copy.deepcopy(self.policy)
        forged['coverage_gaps'] = [g for g in forged['coverage_gaps'] if g['id'] != 'groundhog_sanitizer_execution_contract']
        with self.assertRaisesRegex(s.ContractError, 'coverage gap'):
            layer.before_policy(forged)


if __name__ == '__main__':
    unittest.main()
