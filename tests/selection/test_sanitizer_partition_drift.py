"""Exact groundhog.linux.sanitizers partition drift diagnosis (nostrc-0cfsg.36.22).

Test-only: the reviewed nonmember contract stays exact and still refuses the
hosted fresh export. No renewal, membership, waiver or setup change is applied.
Not execution, hosted or W11 acceptance.
"""
import copy
import hashlib
import json
import re
import unittest
from unittest import mock
from selector_support import ROOT, s
from realization_support import r

WITNESS = 'tests/selection/fixtures/sanitizer_partition_drift_20261024.json'
SHA256 = '443684234eedd500dd91b7432232e6068e484baa2cb1ec3e4dbc22d6c72f6662'
PROFILE = 'groundhog.linux.sanitizers'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'sanitizer partition drift witness changed')
    return json.loads(raw)


class SanitizerPartitionDrift(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.data = witness()
        self.contract = s.diagnostic_nonmember_contracts(self.policy)[PROFILE]
        # Exactly how the engine derives authored primaries (_diagnostic_nonmembers).
        self.authored = [name for check in self.policy['checks'].values()
                         if check.get('kind') == 'ctest' and r._eligible(check, PROFILE)
                         for name in r._membership(check, PROFILE)]

    def partition(self, names, contract=None, closure=None):
        """Run the real partition predicate on hosted registration names.

        The hosted engine raised the partition error, so its preceding
        required-closure equality had passed on the fresh export: hold the
        closure at that hosted-verified value (the export retains no fixture
        properties to recompute it)."""
        contract = contract or self.contract
        closure = list(contract['required_closure'] if closure is None else closure)
        tests = {name: {'name': name, 'properties': []} for name in names}

        def fixture_closure(selected, _tests, _descriptors):
            self.assertEqual(sorted(selected), sorted(self.authored))
            return closure
        with mock.patch.object(r, '_fixture_closure', side_effect=fixture_closure):
            r._nonmember_partition(contract, tests, {}, self.authored)

    def test_reviewed_contract_is_exact_and_unrenewed(self):
        self.assertEqual(s.digest(self.contract), self.data['contract_sha256'])
        self.assertEqual({k: len(self.contract[k]) for k in ('registrations', 'required_closure')},
                         self.data['contract_counts'])
        self.assertEqual(self.data['contract_counts'], {'registrations': 347, 'required_closure': 65})
        self.assertEqual(self.contract['review']['source_sha'], self.data['delta']['review_source']['commit'])
        self.assertNotIn(self.data['delta']['registration'], self.contract['registrations'] + self.contract['required_closure'])
        self.assertNotIn(self.data['delta']['registration'], self.authored)
        # Every exact newer layer leaves the nonmember authority untouched.
        import groundhog_app_enrollment_support as newest
        self.assertEqual(newest.before_policy(self.policy)['diagnostic_nonmembers'], self.policy['diagnostic_nonmembers'])
        self.assertEqual(s.digest(self.policy['profiles'][PROFILE]), self.data['hosted']['profile_hash'])
        self.assertEqual(self.contract['profile_hash'], self.data['hosted']['profile_hash'])

    def test_hosted_failure_is_bound_and_retains_incomplete_accounting(self):
        hosted = self.data['hosted']
        self.assertEqual(hosted['failure_errors'], [{'message': 'diagnostic nonmember fresh registration partition differs',
                                                     'type': 'RealizationError'}])
        self.assertEqual((hosted['failure_stage'], hosted['registration_audit'], hosted['input_audit']),
                         ('resolve', 'not_performed', 'not_completed'))
        self.assertEqual(hosted['work_status'], {'configure': 'passed', 'build': 'not_executed', 'test': 'not_executed'})
        self.assertIs(hosted['complete'], False)
        self.assertIs(hosted['complete_gate_eligible'], False)
        self.assertEqual(hosted['configure_exit_code'], 0)
        self.assertIn('-- Found blueprint-compiler: /usr/bin/blueprint-compiler', hosted['blueprint_configure_lines'])
        self.assertEqual(sorted(self.authored), hosted['sanitizer_members'])
        self.assertTrue(set(self.authored) <= set(self.contract['required_closure']))

    def test_exact_single_registration_delta(self):
        hosted = set(self.data['hosted_registrations'])
        self.assertEqual(len(hosted), 413)
        reviewed = set(self.contract['registrations']) | set(self.contract['required_closure'])
        self.assertFalse(reviewed - hosted)
        self.assertEqual(hosted - reviewed, {self.data['delta']['registration']})

    def test_real_partition_predicate_refuses_hosted_export_and_single_name_is_causal(self):
        hosted = self.data['hosted_registrations']
        delta = self.data['delta']['registration']
        with self.assertRaisesRegex(r.RealizationError, 'fresh registration partition differs'):
            self.partition(hosted)
        # Counterfactual control: without exactly that name the reviewed partition holds.
        self.partition([n for n in hosted if n != delta])
        # Any other unknown, missing or duplicated-into-closure registration still refuses.
        for names in ([n for n in hosted if n != delta] + ['unknown-registration'],
                      [n for n in hosted if n not in (delta, self.contract['registrations'][0])]):
            with self.subTest(size=len(names)), self.assertRaisesRegex(r.RealizationError, 'partition differs'):
                self.partition(names)
        overlapping = copy.deepcopy(self.contract)
        overlapping['registrations'] = sorted(overlapping['registrations'] + [self.contract['required_closure'][0]])
        with self.assertRaisesRegex(r.RealizationError, 'partition differs'):
            self.partition([n for n in hosted if n != delta], contract=overlapping)

    def test_membership_or_unreviewed_renewal_cannot_silently_admit(self):
        hosted = self.data['hosted_registrations']
        delta = self.data['delta']['registration']
        # Owning it as a sanitizer member changes the reviewed 65-entry closure: refuses.
        with self.assertRaisesRegex(r.RealizationError, 'required fixture closure changed'):
            self.partition(hosted, closure=sorted(self.contract['required_closure'] + [delta]))
        # A renewed list would satisfy the predicate, but the contract validator binds
        # review provenance/profile hash; this bead applies no renewal.
        renewed = copy.deepcopy(self.contract)
        renewed['registrations'] = sorted(renewed['registrations'] + [delta])
        self.partition(hosted, contract=renewed)
        self.assertNotEqual(s.digest(renewed), self.data['contract_sha256'])
        forged = copy.deepcopy(self.policy)
        forged['diagnostic_nonmembers'][PROFILE]['review'] = {'source_sha': 'unknown'}
        with self.assertRaises(ValueError):
            s.diagnostic_nonmember_contracts(forged)

    def test_source_and_legacy_setup_facts(self):
        guard = self.data['delta']['cmake_guard']
        text = (ROOT / guard['path']).read_text()
        index = text.index(guard['registration'])
        self.assertIn(guard['condition'], text[text.rfind('\n  if(', 0, index):index])
        flags = self.data['legacy_parity']['configure_flags']
        self.assertEqual(self.policy['profiles'][PROFILE]['configure_args'], flags)
        workflow = (ROOT / self.data['legacy_parity']['workflow']).read_text()
        job = workflow[workflow.index('\n  groundhog-sanitizers:') + 1:]
        following = re.search(r'\n  [A-Za-z0-9_-]+:\n', job)
        job = job[:following.start()] if following else job
        self.assertIn('blueprint-compiler', job)
        for flag in flags[2:]:
            self.assertIn(flag, job, flag)
        hosted_argv = self.data['hosted']['configure_argv']
        self.assertTrue(all(flag in hosted_argv for flag in flags), hosted_argv)

    def test_changed_witness_bytes_refuse(self):
        raw = (ROOT / WITNESS).read_bytes()
        with mock.patch('pathlib.Path.read_bytes', return_value=raw + b' '):
            with self.assertRaisesRegex(ValueError, 'witness changed'):
                witness()


if __name__ == '__main__':
    unittest.main()
