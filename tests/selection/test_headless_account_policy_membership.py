"""Bounded ordinary account/identity policy membership, not gate acceptance."""
import copy
import hashlib
import json
import unittest

from selector_support import ROOT, change, s
import test_selection_realization as r
from headless_account_policy_projection import (
    BASE, EXPORT, FAMILY, RENEWAL, before_account_policy_family,
    verify_historical_account_policy_fixtures,
)

PROFILE = 'groundhog.linux'
CHECKS = {
    'groundhog.account_policy_controller': 'groundhog-account',
    'groundhog.account_policy_relays': 'groundhog-account-relays',
    'groundhog.account_policy_auth': 'groundhog-auth-policy',
    'groundhog.account_policy_identity': 'groundhog-identity',
}
SCHEMA = '{source}/gnome/groundhog/data/org.nostr.Groundhog.gschema.xml'
COUNTS = {'groundhog-account': (27, 176), 'groundhog-account-relays': (26, 173),
          'groundhog-auth-policy': (26, 173), 'groundhog-identity': (24, 168)}


class HeadlessAccountPolicyMembership(unittest.TestCase):
    def setUp(self):
        self.p = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.export = json.loads((ROOT / EXPORT).read_text())
        self.real = self.export['realization']
        self.tests, self.ds = r._test_maps(self.real)
        self.witness = json.loads((ROOT / RENEWAL).read_text())

    def assert_covered(self, path, check):
        path = path.removeprefix('{source}/')
        self.assertTrue((ROOT / path).is_file(), path)
        classified = s.classify(self.p, path)
        self.assertFalse(classified.get('unclassified'), path)
        if classified['effect'] != 'global':
            self.assertEqual(classified['effect'], 'impact', path)
            self.assertTrue(set(classified['nodes']) <=
                            r._impact_prerequisites(self.p, check['nodes']), path)

    def test_exact_checks_and_runtime_build_prerequisites(self):
        self.assertEqual(sorted(CHECKS.values()), FAMILY)
        for ident, name in CHECKS.items():
            check = self.p['checks'][ident]
            targets = ['groundhog-test-schemas', 'test-' + name]
            if name == 'groundhog-identity':
                targets.insert(0, 'groundhog-schema')
            self.assertEqual(check['build_targets'], targets)
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            self.assertEqual(check['runtime_inputs'], [])
            self.assertEqual((check['kind'], check['cost'], check['requirement'],
                              check['expected_outcome']), ('ctest', 'ordinary', 'blocking', 'pass'))
            self.assertEqual([k for k, v in self.p['checks'].items()
                              if name in v.get('ctest_tests', {}).get(PROFILE, [])], [ident])
            self.assertEqual(self.ds[name]['owner'], ['groundhog.unclassified'])
            self.assertEqual(self.ds[name]['inputs'], [SCHEMA])
            self.assertEqual(self.ds[name]['requires'], [])
            self.assertIn('groundhog.resources', check['nodes'])
            self.assertTrue({'groundhog.identity_auth', 'groundhog.account_relays'} &
                            set(check['nodes']))

    def test_exported_command_artifact_and_complete_compiled_input_closure(self):
        for ident, name in CHECKS.items():
            ids = r._target_closure(r._mapped_targets(self.tests[name], self.ds[name], self.real),
                                    self.real)
            targets = sorted(self.real['targets'][key]['name'] for key in ids)
            sources = sorted({entry['path'] for key in ids for entry in
                              self.real['targets'][key]['sources']
                              if entry['compiled'] and not entry['generated']})
            witness = self.export['primary'][name]
            self.assertEqual((len(targets), len(sources)), COUNTS[name])
            self.assertEqual(targets, witness['closure_targets'])
            self.assertEqual(sources, witness['source_inputs'])
            self.assertEqual(self.tests[name]['command'],
                             ['{build}/gnome/groundhog/test-' + name])
            self.assertTrue(set(targets).isdisjoint(
                {'groundhog', 'groundhog-ui', 'groundhog-conversation-ui',
                 'marmot', 'marmot-gobject', 'nostr_gtk', 'nostr_gtk_portable'}))
            self.assertFalse(any('mls' in target or 'mdk' in target for target in targets))
            for path in sources + self.ds[name]['inputs']:
                self.assert_covered(path, self.p['checks'][ident])

    def test_fixture_closure_is_exact_and_headless(self):
        for name in FAMILY:
            closure = r._fixture_closure([name], self.tests, self.ds)
            self.assertEqual(closure, sorted([name, 'groundhog-test-homes',
                                              'groundhog-test-gst-registry']))
            self.assertEqual(closure, self.export['primary'][name]['closure_names'])
            for fixture in closure:
                self.assertEqual(self.ds[fixture]['requires'], [])
        self.assertEqual(r._fixture_closure(FAMILY, self.tests, self.ds),
                         sorted(FAMILY + ['groundhog-test-homes', 'groundhog-test-gst-registry']))

    def test_actual_schema_isolation_timeouts_and_no_skip_contract(self):
        for name in FAMILY:
            props = r._properties(self.tests[name])
            self.assertEqual(props['TIMEOUT'], {'groundhog-account': 30,
                'groundhog-account-relays': 40, 'groundhog-auth-policy': 60,
                'groundhog-identity': 20}[name])
            self.assertNotIn('SKIP_RETURN_CODE', props)
            self.assertEqual(props['FIXTURES_REQUIRED'], ['groundhog-test-homes'])
            env = props['ENVIRONMENT']
            for item in ('GSETTINGS_BACKEND=memory', 'NOSTRC_TEST_ISOLATION=1',
                         'HOME={build}/gnome/groundhog/test-homes/' + name + '/home',
                         'GST_REGISTRY={build}/gnome/groundhog/gst-registry.bin'):
                self.assertIn(item, env)
            schema_dir = '{build}/gnome/groundhog'
            if name != 'groundhog-identity':
                schema_dir += '/test-schemas'
                self.assertIn('G_DEBUG=fatal-criticals' +
                              (',fatal-warnings' if name == 'groundhog-account' else ''), env)
            self.assertIn('GSETTINGS_SCHEMA_DIR=' + schema_dir, env)
            self.assertFalse(any(v.startswith(('DISPLAY=', 'WAYLAND_DISPLAY=',
                                              'DBUS_SESSION_BUS_ADDRESS=')) for v in env))

    def test_identity_header_account_relay_schema_and_test_source_select_consumers(self):
        for path in ('gnome/groundhog/src/identity/gh-identity.c',
                     'gnome/groundhog/src/identity/gh-identity.h',
                     SCHEMA.removeprefix('{source}/')):
            plan = s.make_plan(self.p, [change(path)], head=BASE, profiles=[PROFILE])
            self.assertTrue(set(CHECKS) <= set(plan['selected_checks']), path)
        for ident, path in (
            ('groundhog.account_policy_relays', 'gnome/groundhog/src/app/gh-account-relays.c'),
            ('groundhog.account_policy_controller', 'gnome/groundhog/tests/app/test_account_controller.c'),
            ('groundhog.account_policy_auth', 'gnome/groundhog/tests/app/test_auth_policy.c'),
            ('groundhog.account_policy_identity', 'gnome/groundhog/tests/identity/test_identity.c'),
        ):
            plan = s.make_plan(self.p, [change(path)], head=BASE, profiles=[PROFILE])
            self.assertIn(ident, plan['selected_checks'], path)

    def test_missing_fixture_or_wrong_artifact_refuses(self):
        tests = copy.deepcopy(self.tests)
        for missing in ('groundhog-test-homes', 'groundhog-test-gst-registry'):
            del tests[missing]
        with self.assertRaisesRegex(r.RealizationError, 'missing setup'):
            r._fixture_closure(FAMILY, tests, self.ds)
        name = 'groundhog-account'
        bad = copy.deepcopy(self.tests[name])
        bad['command'] = self.tests['groundhog-identity']['command']
        with self.assertRaisesRegex(r.RealizationError, 'command/descriptor target disagreement'):
            r._mapped_targets(bad, self.ds[name], self.real)

    def test_semantic_schema_and_shared_fixture_edges_are_not_catchall_credit(self):
        mutations = [
            ('groundhog.account_policy_controller', 'nip17',
             '{source}/nips/nip17/src/nip17.c'),
            ('groundhog.account_policy_auth', 'shared.tests',
             '{source}/tests/common/nostrc-test-bus.c'),
            ('groundhog.account_policy_identity', 'groundhog.resources', SCHEMA),
        ]
        for ident, node, path in mutations:
            check = copy.deepcopy(self.p['checks'][ident])
            check['nodes'].remove(node)
            with self.assertRaises(AssertionError):
                self.assert_covered(path, check)
        check = copy.deepcopy(self.p['checks']['groundhog.account_policy_controller'])
        check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
        with self.assertRaises(AssertionError):
            self.assert_covered('{source}/gnome/groundhog/src/identity/gh-identity.c', check)

    def test_exact_four_name_projection_preserves_omission_and_source_authority(self):
        current = self.p['diagnostic_nonmember_subsets'][PROFILE]
        old = before_account_policy_family(current)
        self.assertEqual((len(current['primary_registrations']), len(current['required_closure'])),
                         (34, 36))
        self.assertEqual((len(old['primary_registrations']), len(old['required_closure'])), (30, 32))
        self.assertEqual(len(current['registrations']), 33)
        self.assertEqual(len(self.export['baseline_omission_build_targets']), 29)
        self.assertEqual(len(current['review']['source_inputs']), 193)
        self.assertTrue(set(FAMILY).isdisjoint(current['registrations']))
        authored = sorted(n for c in self.p['checks'].values()
                          for n in c.get('ctest_tests', {}).get(PROFILE, []))
        self.assertEqual(current['primary_registrations'], authored)
        from nip44_vector_projection import before_nip44_vector_renewal
        previous_wave = before_nip44_vector_renewal(current, PROFILE)
        for key in current.keys() - {'primary_registrations', 'required_closure'}:
            self.assertEqual(previous_wave[key], old[key], key)
        self.assertFalse(self.p['enforcement_ready'])
        for key in ('candidate_contract_hash', 'profile_hash', 'review', 'registration_universe'):
            bad = copy.deepcopy(current)
            bad[key] = 'changed'
            with self.assertRaisesRegex(s.ContractError, 'non-selection authority'):
                before_account_policy_family(bad)
        for key in ('primary_registrations', 'required_closure'):
            bad = copy.deepcopy(current)
            bad[key].append('groundhog-account-store')
            with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
                before_account_policy_family(bad)
        with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
            before_account_policy_family(old)

    def test_historical_bytes_and_live_source_controls_remain_exact(self):
        self.assertEqual(self.export['source_identity'], {'kind': 'committed', 'sha': BASE})
        self.assertEqual(self.witness['source_identity'], self.export['source_identity'])
        self.assertEqual(self.export['profile'], self.p['profiles'][PROFILE])
        self.assertEqual(self.witness['added_primary'], FAMILY)
        verify_historical_account_policy_fixtures()
        for path, expected in self.p['diagnostic_nonmember_subsets'][PROFILE]['review']['source_inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected, path)


if __name__ == '__main__':
    unittest.main()
