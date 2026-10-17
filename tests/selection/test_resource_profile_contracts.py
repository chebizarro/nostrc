"""New resource registrations are distinct from legacy signer-only selection."""
import hashlib
import json
import unittest

from selector_support import ROOT, change, s


class ResourceProfileContracts(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.export = json.loads((ROOT / 'tests/selection/fixtures/resource_profile_exports.json').read_text())

    def test_fresh_core_export_preserves_app_home_fixture_and_targetless_commands(self):
        self.assertEqual(self.export['architecture'], 'aarch64')
        self.assertFalse(self.export['authored_x86_64_execution_acceptance'])
        self.assertEqual(self.export['primary_names'], ['gnostr-resource-build', 'nostr-gtk-resource-build'])
        self.assertEqual(self.export['fixture_names'], ['gnostr-test-homes'])
        registrations = {test['name']: test for test in self.export['ctest']}
        for ident, name, owner, count in (
                ('gnostr.resource_build', 'gnostr-resource-build', 'gnostr', 100),
                ('nostr.gtk.resource_build', 'nostr-gtk-resource-build', 'nostr.gtk', 17)):
            check = self.policy['checks'][ident]
            self.assertEqual(check['ctest_tests'], {'core.linux': [name]})
            self.assertEqual(check['profiles'], ['core.linux'])
            self.assertEqual(check['build_targets'], [])
            descriptor = self.export['descriptors'][name]
            self.assertEqual(descriptor['owner'], [owner])
            self.assertEqual(descriptor['owner_authority'], 'descriptor')
            self.assertEqual(descriptor['targets'], [])
            self.assertEqual(descriptor['requires'], [])
            self.assertEqual(descriptor['expected'], ['pass'])
            self.assertEqual(len(descriptor['inputs']), count)
            self.assertIn('{source}/tests/common/test_blueprint_resources.py', descriptor['inputs'])
            self.assertIn('{source}/cmake/NostrcGtkResources.cmake', descriptor['inputs'])
            self.assertIn('--component', registrations[name]['command'])
        properties = {item['name']: item['value']
                      for item in registrations['gnostr-resource-build']['properties']}
        self.assertEqual(properties['FIXTURES_REQUIRED'], ['gnostr-test-homes'])
        self.assertEqual(properties['DEPENDS'], ['gnostr-test-homes'])

    def test_resource_sources_select_real_checks_without_live_reference_checks(self):
        for path, required in (
                ('apps/gnostr/data/ui/gnostr-main-window.blp', ['gnostr.resource_build']),
                ('nostr-gtk/data/ui/gnostr-profile-pane.blp', ['nostr.gtk.resource_build']),
                ('cmake/NostrcGtkResources.cmake', ['gnostr.resource_build', 'nostr.gtk.resource_build']),
                ('tests/common/test_blueprint_resources.py', ['gnostr.resource_build', 'nostr.gtk.resource_build'])):
            with self.subTest(path=path):
                plan = s.make_plan(self.policy, [change(path)], head='a' * 40, base='b' * 40)
                self.assertTrue(set(required) <= set(plan['selected_checks']))
                self.assertFalse(any(name.startswith(('mdk08.', 'mdk011.'))
                                     for name in plan['selected_checks']))

    def test_registration_sources_are_exact_audited_contract_inputs(self):
        inputs = self.policy['checks']['selection.contract']['runtime_inputs']
        for relative, expected in self.export['source_sha256'].items():
            self.assertIn('{source}/' + relative, inputs)
            self.assertEqual(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest(), expected, relative)

    def test_new_gtk_registration_is_visible_but_not_in_legacy_grotto_selection(self):
        exports = json.loads((ROOT / 'tests/selection/fixtures/grotto_profile_exports.json').read_text())
        for variant in exports['variants'].values():
            self.assertIn('nostr-gtk-resource-build', variant['all_registration_names'])
            self.assertNotIn('gnostr-resource-build', variant['all_registration_names'])
            self.assertNotIn('nostr-gtk-resource-build', variant['primary_names'])
        self.assertEqual(set(self.policy['diagnostic_nonmembers']), {'groundhog.linux.sanitizers'})


if __name__ == '__main__':
    unittest.main()
