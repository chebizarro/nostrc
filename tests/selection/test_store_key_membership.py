"""Ordinary Groundhog membership from real exports, not full-gate acceptance."""
import hashlib
import json
import re
import unittest

from profile_contract_support import step_script
from selector_support import ROOT, change, s

CHECKS = {
    'sanitizer.groundhog_store_key': 'groundhog-store-key',
    'sanitizer.groundhog_store_key_keyring': 'groundhog-store-key-keyring',
}
PROFILE = 'groundhog.linux'
FIXTURE = 'tests/selection/fixtures/store_key_membership_exports.json'


class _MembershipFixture:
    def setUp(self):
        self.current_policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.policy = self.current_policy
        self.export = json.loads((ROOT / FIXTURE).read_text())

    def names(self, profile, checks=None):
        return [name for ident, check in self.policy['checks'].items()
                if check['kind'] == 'ctest' and (checks is None or ident in checks)
                for name in check.get('ctest_tests', {}).get(profile, [])]

    def workflow_filter(self):
        script = step_script((ROOT / '.github/workflows/groundhog-ci.yml').read_text(),
                             'Run all Groundhog tests under a controlled display')
        return (re.search(r"-R\s+'([^']+)'", script).group(1),
                re.search(r"-E\s+'([^']+)'", script).group(1))

    def assert_classification_covered(self, owned, covered, path):
        self.assertIs(owned.get('unclassified'), False, path)
        if owned['effect'] == 'global':
            return
        self.assertEqual(owned['effect'], 'impact', path)
        nodes = set(owned['nodes'])
        self.assertTrue(nodes, path)
        self.assertTrue(nodes <= self.policy['nodes'].keys(), path)
        self.assertTrue(nodes <= covered, path)



class StoreKeyMembership(_MembershipFixture, unittest.TestCase):
    def test_exact_existing_checks_gain_only_ordinary_groundhog_membership(self):
        for ident, name in CHECKS.items():
            check = self.policy['checks'][ident]
            self.assertEqual(set(check['profiles']), {PROFILE, 'groundhog.linux.sanitizers'})
            self.assertEqual(set(check['ctest_tests']), set(check['profiles']))
            for profile in check['profiles']:
                self.assertEqual(check['ctest_tests'][profile], [name])
                self.assertEqual(self.names(profile).count(name), 1)
            self.assertEqual(check['expected_outcome'], 'pass')
            self.assertEqual(check['runtime_inputs'], [])

    def test_workflow_filter_and_profile_contract_are_unchanged(self):
        include, exclude = self.workflow_filter()
        for name in CHECKS.values():
            self.assertRegex(name, include)
            self.assertNotRegex(name, exclude)
        profile = self.policy['profiles'][PROFILE]
        self.assertEqual(profile['instrumentation'], 'none')
        self.assertTrue({'display', 'private_bus'} <= set(profile['capabilities']))
        self.assertEqual(profile['environment'], {
            'NOSTRC_TEST_NIP5F_APPROVAL': 'unidentified-get-public-key-once'})
        self.assertEqual(self.export['actual_architecture'], 'aarch64')
        self.assertFalse(self.export['authored_x86_64_execution_acceptance'])

    def test_unrecognized_or_empty_input_classification_is_rejected(self):
        covered = set(self.policy['nodes']) | {'unknown.node'}
        cases = [s.classify(self.policy, 'unowned-store-key-input.c'),
                 {'effect': 'impact', 'nodes': [], 'unclassified': False},
                 {'effect': 'impact', 'nodes': ['unknown.node'], 'unclassified': False}]
        for owned in cases:
            with self.subTest(owned=owned), self.assertRaises(AssertionError):
                self.assert_classification_covered(owned, covered, 'negative-input')

    def test_selection_contract_tracks_the_new_test_and_export(self):
        inputs = self.policy['checks']['selection.contract']['runtime_inputs']
        self.assertIn('{source}/' + FIXTURE, inputs)
        self.assertIn('{source}/tests/selection/test_store_key_membership.py', inputs)



class SourceClosure(_MembershipFixture, unittest.TestCase):
    """Retained native export/input matrix; direct/manual discovery only."""
    def test_actual_target_source_and_fixture_inputs_reach_each_check(self):
        for ident, name in CHECKS.items():
            exported = self.export['primary'][name]
            self.assertIn('groundhog-store-key', exported['closure_targets'])
            self.assertIn('test-' + name, exported['closure_targets'])
            self.assertEqual(exported['closure_names'],
                             [name, 'groundhog-test-gst-registry', 'groundhog-test-homes'])
            covered = set(self.policy['checks'][ident]['nodes'])
            pending = list(covered)
            while pending:
                for node in self.policy['nodes'][pending.pop()].get('depends_on', []):
                    if node not in covered:
                        covered.add(node)
                        pending.append(node)
            for path in exported['source_inputs'] + exported['runtime_inputs']:
                if path.startswith('{build}/'):
                    self.assertEqual(path, '{build}/gnome/groundhog/groundhog-test-homes.cmake')
                    continue
                path = path.removeprefix('{source}/')
                self.assertTrue((ROOT / path).is_file(), path)
                owned = s.classify(self.policy, path)
                self.assert_classification_covered(owned, covered, path)
            props = self.export['properties'][name]
            self.assertEqual(props['FIXTURES_REQUIRED'], ['groundhog-test-homes'])
            self.assertIn('NOSTRC_TEST_ISOLATION=1', props['ENVIRONMENT'])
        self.assertEqual(self.export['properties']['groundhog-store-key']['TIMEOUT'], 30)
        self.assertEqual(self.export['properties']['groundhog-store-key-keyring']['TIMEOUT'], 90)
        self.assertEqual(self.export['properties']['groundhog-store-key-keyring']['SKIP_RETURN_CODE'], 77)
        self.assertIn('nostrc-test-bus', self.export['primary']['groundhog-store-key-keyring']['closure_targets'])



if __name__ == '__main__':
    unittest.main()
