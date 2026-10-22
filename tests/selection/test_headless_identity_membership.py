"""Exact pure remote-identity/account family; retained exports are not acceptance."""
import copy
import json
import unittest

from headless_identity_projection import FAMILY, RENEWAL, before_identity_family
from selector_support import ROOT, change, s
from realization_support import r

PROFILE = 'groundhog.linux'
EXPORT = 'tests/selection/fixtures/headless_identity_exports.json'
CHECKS = {
    'groundhog.identity_account_backends': ('groundhog-account-backends', 'test-groundhog-account-backends'),
    'groundhog.identity_account_remote_switch': ('groundhog-account-remote-switch', 'test-groundhog-account-remote-switch'),
    'groundhog.identity_credentials_list': ('groundhog-nip46-credentials-list', 'test-groundhog-nip46-credentials-list'),
    'groundhog.identity_nip46_session': ('groundhog-nip46-session', 'test-groundhog-nip46-session'),
    'groundhog.identity_signer_nip46': ('groundhog-signer-nip46', 'test-groundhog-signer-nip46'),
}


class HeadlessIdentityMembership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.export = json.loads((ROOT / EXPORT).read_text())
        self.real = self.export['realization']
        self.tests, self.descriptors = r._test_maps(self.real)

    def covered(self, check):
        nodes = set(check['nodes'])
        pending = list(nodes)
        while pending:
            for node in self.policy['nodes'][pending.pop()].get('depends_on', []):
                if node not in nodes:
                    nodes.add(node)
                    pending.append(node)
        return nodes

    def assert_input_covered(self, path, check):
        path = path.removeprefix('{source}/')
        self.assertTrue((ROOT / path).is_file(), path)
        classified = s.classify(self.policy, path)
        self.assertFalse(classified.get('unclassified'), path)
        if classified['effect'] != 'global':
            self.assertEqual(classified['effect'], 'impact', path)
            self.assertTrue(classified['nodes'], path)
            self.assertTrue(set(classified['nodes']) <= self.covered(check), path)

    def test_exact_ordinary_family_and_exclusions(self):
        self.assertEqual(sorted(name for name, _ in CHECKS.values()), FAMILY)
        for ident, (name, target) in CHECKS.items():
            check = self.policy['checks'][ident]
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            self.assertEqual(check['build_targets'], [target])
            self.assertEqual(check['kind'], 'ctest')
            self.assertEqual(check['expected_outcome'], 'pass')
            self.assertEqual(check['runtime_inputs'], [])
            owners = [key for key, item in self.policy['checks'].items()
                      if name in item.get('ctest_tests', {}).get(PROFILE, [])]
            self.assertEqual(owners, [ident])
        ordinary = {name for check in self.policy['checks'].values()
                    for name in check.get('ctest_tests', {}).get(PROFILE, [])}
        excluded = {'groundhog-identity', 'groundhog-account', 'groundhog-auth-policy',
                    'groundhog-account-relays', 'groundhog-account-store', 'groundhog-account-ui',
                    'groundhog-nip46-wire', 'groundhog-nip46-nak',
                    'groundhog-nip46-credentials-keyring', 'groundhog-nip46-credentials-keychain'}
        self.assertTrue(ordinary.isdisjoint(excluded))
        self.assertEqual(self.policy['profiles'][PROFILE], self.export['profile'])
        self.assertFalse(self.policy['enforcement_ready'])
        for node in ('groundhog.unclassified', 'execution.groundhog'):
            self.assertEqual(self.policy['nodes'][node]['depends_on'], [])

    def test_real_command_file_api_descriptor_and_fixture_closures(self):
        counts = [24, 24, 22, 19, 21]
        for (ident, (name, target)), count in zip(CHECKS.items(), counts):
            check = self.policy['checks'][ident]
            desc = self.descriptors[name]
            self.assertEqual(desc['requires'], [])
            self.assertEqual(desc['expected'], ['pass'])
            self.assertIn('groundhog.unclassified', desc['owner'])
            self.assertTrue(set(desc['owner']) <= self.covered(check))
            ids = r._target_closure(r._mapped_targets(self.tests[name], desc, self.real), self.real)
            targets = sorted(self.real['targets'][key]['name'] for key in ids)
            self.assertEqual(targets, self.export['primary'][name]['closure_targets'])
            self.assertEqual(len(targets), count)
            self.assertIn(target, targets)
            self.assertTrue(set(targets).isdisjoint({'groundhog', 'groundhog-ui', 'marmot', 'nostr_gtk'}))
            sources = sorted({entry['path'] for key in ids for entry in self.real['targets'][key]['sources']
                              if entry['compiled'] and not entry['generated']})
            self.assertEqual(sources, self.export['primary'][name]['source_inputs'])
            for path in sources + desc['inputs']:
                self.assert_input_covered(path, check)
            fixtures = r._fixture_closure([name], self.tests, self.descriptors)
            self.assertEqual(fixtures, sorted([name, 'groundhog-test-homes', 'groundhog-test-gst-registry']))
            self.assertEqual(fixtures, self.export['primary'][name]['closure_names'])

    def test_runtime_schema_isolation_and_fatal_guards_are_retained(self):
        for name in FAMILY:
            props = r._properties(self.tests[name])
            self.assertEqual(props['TIMEOUT'], 30)
            self.assertEqual(props['FIXTURES_REQUIRED'], ['groundhog-test-homes'])
            self.assertNotIn('SKIP_RETURN_CODE', props)
            self.assertIn('NOSTRC_TEST_ISOLATION=1', props['ENVIRONMENT'])
            self.assertIn('GSETTINGS_BACKEND=memory', props['ENVIRONMENT'])
            debug = [item for item in props['ENVIRONMENT'] if item.startswith('G_DEBUG=')]
            expected = ([] if name in ('groundhog-nip46-session', 'groundhog-signer-nip46')
                        else ['G_DEBUG=fatal-criticals'] if name == 'groundhog-nip46-credentials-list'
                        else ['G_DEBUG=fatal-criticals,fatal-warnings'])
            self.assertEqual(debug, expected)
        for name in ('groundhog-account-backends', 'groundhog-account-remote-switch'):
            self.assertIn('groundhog-test-schemas', self.descriptors[name]['targets'])
            self.assertIn('{source}/gnome/groundhog/data/org.nostr.Groundhog.gschema.xml',
                          self.descriptors[name]['inputs'])
        for name in ('groundhog-test-homes', 'groundhog-test-gst-registry'):
            self.assertEqual(self.descriptors[name]['owner'], ['testing.framework'])
            self.assertEqual(self.descriptors[name]['expected'], ['fixture'])

    def test_wrong_artifact_or_missing_setup_refuses(self):
        name = 'groundhog-nip46-session'
        bad = copy.deepcopy(self.tests[name])
        bad['command'] = ['{build}/unmapped-session']
        with self.assertRaisesRegex(r.RealizationError, 'lacks explicit inputs/targets'):
            r._mapped_targets(bad, self.descriptors[name], self.real)
        tests = copy.deepcopy(self.tests)
        del tests['groundhog-test-homes']
        del tests['groundhog-test-gst-registry']
        with self.assertRaisesRegex(r.RealizationError, 'missing setup'):
            r._fixture_closure([name], tests, self.descriptors)

    def test_semantic_ownership_is_not_replaced_by_catchall(self):
        check = copy.deepcopy(self.policy['checks']['groundhog.identity_nip46_session'])
        check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
        with self.assertRaises(AssertionError):
            self.assert_input_covered('{source}/gnome/groundhog/src/identity/gh-nip46-session.c', check)
        for path in ('gnome/groundhog/data/org.nostr.Groundhog.gschema.xml',
                     'nostr-gtk/data/nostr-gtk.gresource.xml'):
            plan = s.make_plan(self.policy, [change(path)], head='a' * 40)
            self.assertFalse(any('mdk' in lane for lane in plan['expected_lane_ids']))

    def test_selected_closure_renewal_preserves_all_other_authority(self):
        import test_selection_subset as subset
        contract = subset.contracts(self.policy)[PROFILE]
        previous = before_identity_family(contract)
        renewal = json.loads((ROOT / RENEWAL).read_text())
        authored = sorted(name for check in self.policy['checks'].values()
                          for name in check.get('ctest_tests', {}).get(PROFILE, []))
        self.assertEqual(contract['primary_registrations'], authored)
        self.assertEqual(contract['registrations'], list(subset.REGISTRATIONS))
        self.assertEqual(len(contract['registrations']), 33)
        self.assertEqual(contract['candidate_contract_hash'], renewal['candidate_contract_hash'])
        executions = r._fixture_closure(FAMILY, self.tests, self.descriptors)
        self.assertTrue(set(executions) <= set(contract['required_closure']))
        self.assertFalse(set(executions) <= set(previous['required_closure']))
        self.assertTrue(set(executions).isdisjoint(contract['registrations']))
        for key in ('build', 'test'):
            self.assertEqual(renewal[key]['exit_code'], 0)
        self.assertEqual(renewal['headless_environment'], {
            'DISPLAY': None, 'WAYLAND_DISPLAY': None, 'DBUS_SESSION_BUS_ADDRESS': None})
        self.assertFalse(any(renewal['limits'].values()))
        for key in ('candidate_contract_hash', 'profile_hash', 'review'):
            bad = copy.deepcopy(contract)
            bad[key] = 'unknown-authority-change'
            with self.assertRaisesRegex(s.ContractError, 'non-selection authority'):
                before_identity_family(bad)
        bad = copy.deepcopy(contract)
        bad['required_closure'].append('groundhog-identity')
        with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
            before_identity_family(bad)

    def test_all_new_files_are_selection_contract_inputs(self):
        inputs = self.policy['checks']['selection.contract']['runtime_inputs']
        for path in (EXPORT, RENEWAL, 'tests/selection/headless_identity_projection.py',
                     'tests/selection/test_headless_identity_membership.py'):
            self.assertIn('{source}/' + path, inputs)


if __name__ == '__main__':
    unittest.main()
