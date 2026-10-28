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
        self.current_policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.p = self.current_policy
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
        expected = {
            'gnome/groundhog/src/identity/gh-identity.c': {
                'groundhog.account_policy_auth', 'groundhog.account_policy_identity'},
            'gnome/groundhog/src/identity/gh-identity.h': {
                'groundhog.account_policy_auth', 'groundhog.account_policy_identity',
                'groundhog.account_policy_relays'},
            SCHEMA.removeprefix('{source}/'): set(CHECKS),
        }
        for path, checks in expected.items():
            plan = s.make_plan(self.p, [change(path)], head=BASE, profiles=[PROFILE])
            self.assertEqual(set(CHECKS) & set(plan['selected_checks']), checks, path)
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
            check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
            with self.assertRaises(AssertionError):
                self.assert_covered(path, check)
        check = copy.deepcopy(self.p['checks']['groundhog.account_policy_controller'])
        check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
        with self.assertRaises(AssertionError):
            self.assert_covered('{source}/gnome/groundhog/src/identity/gh-identity.c', check)




if __name__ == '__main__':
    unittest.main()
