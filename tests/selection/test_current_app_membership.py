"""Current app ownership and selection, not historical enrollment inverses."""
import json
import unittest

from selector_support import ROOT, change, s


APP_CHECKS = {
    'supported.apps.blossom.cache', 'supported.apps.grelay.tests',
    'supported.apps.grotto', 'supported.apps.grotto.native.host',
    'supported.apps.grotto.native.host.private_bus', 'supported.apps.grotto.tests',
    'supported.apps.grotto.tests.display', 'supported.apps.grotto.tests.display.private_bus',
    'supported.apps.grotto.tests.private_bus',
}

# Suites deleted with the module they tested; no current check may claim them.
RETIRED_SUITES = {'signer/hsm-provider', 'signer/social-recovery'}


class CurrentAppMembership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')

    def test_app_membership_is_unique_and_matches_ordinary_peer(self):
        for family, profile, peer in (
                ('coreasan', 'core.linux.asan', 'core.linux'),
                ('groundhog', 'groundhog.linux', 'groundhog.mdk08'),
                ('mdk011', 'groundhog.mdk011', 'groundhog.mdk08')):
            with self.subTest(profile=profile):
                # Retained names are a regression minimum, not a current export
                # or a claim that the historical hosted run was re-executed.
                data = json.loads((ROOT / ('tests/selection/fixtures/' + family +
                                          '_app_enrollment_20261024.json')).read_text())
                claims = {}
                for ident, check in self.policy['checks'].items():
                    for name in check.get('ctest_tests', {}).get(profile, []):
                        self.assertNotIn(name, claims, (profile, name))
                        claims[name] = ident
                # W4 replaces the retired dashboard model without rewriting the
                # historical enrollment receipt; nostrc-83in5 deleted the unwired
                # HSM and social-recovery modules together with their suites.
                current = {'signer/panel-inbox-model' if name == 'signer/home-model' else name
                           for name in data['registrations']} - RETIRED_SUITES
                self.assertTrue(current <= claims.keys())
                self.assertFalse(RETIRED_SUITES & claims.keys())
                for ident in APP_CHECKS:
                    check = self.policy['checks'][ident]
                    self.assertEqual(check['ctest_tests'][profile], check['ctest_tests'][peer])
                    self.assertTrue(set(check.get('requires', [])) <=
                                    set(self.policy['profiles'][profile]['capabilities']))
                    for name in check['ctest_tests'][profile]:
                        self.assertEqual(claims[name], ident)
                self.assertFalse(set(data['support_closure']) & claims.keys())

    def test_grotto_panel_inbox_replaces_retired_dashboard(self):
        cmake = (ROOT / 'apps/grotto/tests/CMakeLists.txt').read_text()
        self.assertIn('add_executable(test-panel-inbox-model ', cmake)
        self.assertIn('add_test(NAME signer/panel-inbox-model COMMAND test-panel-inbox-model)', cmake)
        self.assertNotIn('test-home-dashboard', cmake)
        for ident, check in self.policy['checks'].items():
            self.assertNotIn('test-home-dashboard', check.get('build_targets', []), ident)
            self.assertNotIn('test-home-model', check.get('build_targets', []), ident)
            for names in check.get('ctest_tests', {}).values():
                self.assertNotIn('signer/home-model', names, ident)
        check = self.policy['checks']['supported.apps.grotto.tests.private_bus']
        self.assertIn('test-panel-inbox-model', check['build_targets'])
        for profile, names in check['ctest_tests'].items():
            self.assertIn('signer/panel-inbox-model', names, profile)
        plan = s.make_plan(self.policy, [change('apps/grotto/src/home-model.c')],
                           head='a' * 40, profiles=['core.linux'])
        self.assertIn('supported.apps.grotto.tests.private_bus', plan['selected_checks'])
        self.assertFalse(any(check.startswith('libgo.') for check in plan['selected_checks']))

    def test_groundhog_default_build_owns_app_nodes_and_nip65(self):
        nodes = set(self.policy['checks']['groundhog.default_build']['nodes'])
        self.assertTrue({'blossom_cache', 'grelay', 'grotto.daemon', 'grotto.gui',
                         'grotto.native_host', 'relayctl', 'nip65', 'nip65.test_inputs'} <= nodes)
        self.assertEqual(self.policy['nodes']['groundhog.unclassified']['depends_on'], [])
        self.assertEqual(self.policy['nodes']['execution.groundhog']['depends_on'], [])

    def test_app_source_selects_app_checks_not_unrelated_libgo_suite(self):
        plan = s.make_plan(self.policy, [change('apps/grotto/src/main.c')],
                           head='a' * 40, profiles=['groundhog.linux'])
        self.assertIn('supported.apps.grotto', plan['selected_checks'])
        self.assertFalse(any(check.startswith('libgo.') for check in plan['selected_checks']))

    def test_repaired_core_asan_tests_are_required_without_exclusions(self):
        names = {name for check in self.policy['checks'].values()
                 for name in check.get('ctest_tests', {}).get('core.linux.asan', [])}
        self.assertTrue({'MockRelayUnit', 'concurrency_subscription_shutdown_asan',
                         'test_json_envelope'} <= names)
        workflow = (ROOT / '.github/workflows/nostrc-ci.yml').read_text()
        self.assertNotIn('-E "test_json_envelope|concurrency_subscription_shutdown_asan|MockRelayUnit"', workflow)
        for profile in ('core.linux.asan', 'core.macos.asan'):
            self.assertNotIn('-E', self.policy['profiles'][profile]['workflow_contract']['test_command'])


if __name__ == '__main__':
    unittest.main()
