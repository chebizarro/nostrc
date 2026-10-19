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


class StoreKeyMembership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
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
        self.assertEqual(s.digest(profile), self.export['authored_profile_hash'])
        self.assertEqual(profile['instrumentation'], 'none')
        self.assertEqual(profile['capabilities'], ['display', 'private_bus'])
        self.assertEqual(profile['environment'], {})
        self.assertEqual(self.export['actual_architecture'], 'aarch64')
        self.assertFalse(self.export['authored_x86_64_execution_acceptance'])

    def assert_classification_covered(self, owned, covered, path):
        self.assertIs(owned.get('unclassified'), False, path)
        if owned['effect'] == 'global':
            return
        self.assertEqual(owned['effect'], 'impact', path)
        nodes = set(owned['nodes'])
        self.assertTrue(nodes, path)
        self.assertTrue(nodes <= self.policy['nodes'].keys(), path)
        self.assertTrue(nodes <= covered, path)

    def test_unrecognized_or_empty_input_classification_is_rejected(self):
        covered = set(self.policy['nodes']) | {'unknown.node'}
        cases = [s.classify(self.policy, 'unowned-store-key-input.c'),
                 {'effect': 'impact', 'nodes': [], 'unclassified': False},
                 {'effect': 'impact', 'nodes': ['unknown.node'], 'unclassified': False}]
        for owned in cases:
            with self.subTest(owned=owned), self.assertRaises(AssertionError):
                self.assert_classification_covered(owned, covered, 'negative-input')

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

    def test_store_source_savings_preserve_preexisting_mdk_dependency_paths(self):
        selected = s.make_plan(self.policy, [change('gnome/groundhog/src/store/gh-store-key.c')],
                               head='a' * 40)
        self.assertEqual(selected['effective_mode'], 'affected')
        self.assertEqual(selected['expected_lane_ids'], self.export['baseline_store_source_lanes'])
        self.assertTrue(set(CHECKS) <= set(selected['selected_checks']))
        self.assertFalse(any(p.startswith(('grotto.', 'libgo.', 'core.'))
                             for p in selected['expected_lane_ids']))
        # These MDK lanes already consume the store; this repair must not hide them.
        self.assertIn('groundhog.mdk08', selected['expected_lane_ids'])
        self.assertIn('groundhog.mdk011', selected['expected_lane_ids'])
        ordinary = self.names(PROFILE, selected['selected_checks'])
        self.assertTrue(set(CHECKS.values()) <= set(ordinary))
        self.assertNotIn('groundhog-shell', ordinary)
        self.assertNotIn('groundhog-packaging-version', ordinary)
        self.assertEqual(self.names('grotto.linux', selected['selected_checks']), [])
        self.assertEqual(len(self.names('grotto.linux')), 53)

    def test_libgo_presence_does_not_authorize_cross_profile_execution(self):
        include, _ = self.workflow_filter()
        for profile, evidence in self.export['libgo_boundary'].items():
            self.assertEqual(len(evidence['names']), 33)
            regex = include if profile == PROFILE else self.policy['profiles'][profile]['workflow_contract']['test_command'][5]
            self.assertFalse(any(re.search(regex, name) for name in evidence['names']))
            self.assertTrue(set(evidence['names']).isdisjoint(self.names(profile)))
        self.assertFalse(self.policy['enforcement_ready'])

    def test_export_provenance_keeps_historical_tooling_and_unchanged_graph_sources(self):
        # This witness predates the approved ordinary-subset implementation.
        # Preserve its tooling hashes, not a claim that it attests today's engine.
        historical_tooling = {
            'scripts/test_selection.py': '9554e1d25685d0e4ddd1db30b0257e62c5378a14e25b5ed610ecc571557769c6',
            'scripts/test_selection_realization.py': '7a4a645c905ea2e3b9de8a0755026d98317d2854b7a0f41752d4f5c75f38b489',
        }
        self.assertEqual(self.export['source_identity'], {'kind': 'committed', 'sha': 'fb7335cb0e9546463293b6adf2044fe54178f5ac'})
        self.assertTrue(historical_tooling.keys() <= self.export['source_file_sha256'].keys())
        for path, expected in self.export['source_file_sha256'].items():
            if path in historical_tooling:
                self.assertEqual(expected, historical_tooling[path], path)
            else:
                self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected, path)

    def test_selection_contract_tracks_the_new_test_and_export(self):
        inputs = self.policy['checks']['selection.contract']['runtime_inputs']
        self.assertIn('{source}/' + FIXTURE, inputs)
        self.assertIn('{source}/tests/selection/test_store_key_membership.py', inputs)


if __name__ == '__main__':
    unittest.main()
