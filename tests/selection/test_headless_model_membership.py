"""Exact G02/G05 headless family; real exports, not gate acceptance."""
import copy
import json
import re
import unittest

from profile_contract_support import step_script
from selector_support import ROOT, change, s
from realization_support import r

PROFILE = 'groundhog.linux'
FIXTURE = 'tests/selection/fixtures/headless_model_exports.json'
CHECKS = {
    'groundhog.model_store': 'groundhog-store',
    'groundhog.model_conversations': 'groundhog-conversations',
    'groundhog.model_store_conversations': 'groundhog-store-conversations',
}


class HeadlessModelMembership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.export = json.loads((ROOT / FIXTURE).read_text())
        self.real = self.export['realization']
        self.tests, self.descriptors = r._test_maps(self.real)

    def covered(self, check):
        covered = set(check['nodes'])
        pending = list(covered)
        while pending:
            for node in self.policy['nodes'][pending.pop()].get('depends_on', []):
                if node not in covered:
                    covered.add(node)
                    pending.append(node)
        return covered

    def assert_input_covered(self, path, check):
        path = path.removeprefix('{source}/')
        self.assertTrue((ROOT / path).is_file(), path)
        owned = s.classify(self.policy, path)
        self.assertIs(owned.get('unclassified'), False, path)
        if owned['effect'] != 'global':
            self.assertEqual(owned['effect'], 'impact', path)
            self.assertTrue(owned['nodes'], path)
            self.assertTrue(set(owned['nodes']) <= self.covered(check), path)

    def test_exact_three_ordinary_registrations_without_cross_profile_enrollment(self):
        for ident, name in CHECKS.items():
            check = self.policy['checks'][ident]
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            self.assertEqual(check['build_targets'], ['test-' + name])
            self.assertEqual(check['kind'], 'ctest')
            self.assertEqual(check['expected_outcome'], 'pass')
            self.assertEqual(check['runtime_inputs'], [])
            owners = [key for key, item in self.policy['checks'].items()
                      if name in item.get('ctest_tests', {}).get(PROFILE, [])]
            self.assertEqual(owners, [ident])
        self.assertFalse(self.policy['enforcement_ready'])
        self.assertEqual(self.policy['nodes']['groundhog.unclassified']['depends_on'], [])
        self.assertEqual(self.policy['nodes']['execution.groundhog']['depends_on'], [])

    def test_real_executable_source_and_fixture_closures(self):
        for ident, name in CHECKS.items():
            check = self.policy['checks'][ident]
            desc = self.descriptors[name]
            # These existing conservative descriptors are not silently relabeled.
            self.assertEqual(desc['owner'], ['groundhog.unclassified'])
            self.assertEqual(desc['requires'], [])
            self.assertEqual(desc['expected'], ['pass'])
            mapped = r._mapped_targets(self.tests[name], desc, self.real)
            ids = r._target_closure(mapped, self.real)
            names = sorted(self.real['targets'][key]['name'] for key in ids)
            self.assertEqual(names, self.export['primary'][name]['closure_targets'])
            self.assertIn('test-' + name, names)
            self.assertNotIn('groundhog', names)
            self.assertNotIn('groundhog-ui', names)
            self.assertNotIn('groundhog-identity', names)
            self.assertNotIn('marmot', names)
            closure = r._fixture_closure([name], self.tests, self.descriptors)
            self.assertEqual(closure, sorted([name, 'groundhog-test-homes', 'groundhog-test-gst-registry']))
            self.assertEqual(closure, self.export['primary'][name]['closure_names'])
            sources = sorted({entry['path'] for key in ids
                              for entry in self.real['targets'][key]['sources']
                              if entry['compiled'] and not entry['generated']})
            self.assertEqual(sources, self.export['primary'][name]['source_inputs'])
            for path in sources:
                self.assert_input_covered(path, check)
            self.assertTrue(set(desc['owner']) <= self.covered(check))
        self.assertEqual(len(self.export['primary']['groundhog-store']['closure_targets']), 2)
        for name in ('groundhog-conversations', 'groundhog-store-conversations'):
            self.assertIn('nostr_gtk_portable', self.export['primary'][name]['closure_targets'])
            self.assertNotIn('nostr_gtk', self.export['primary'][name]['closure_targets'])

    def test_runtime_fixture_inputs_and_safety_properties_are_retained(self):
        for name, timeout in [('groundhog-store', 120), ('groundhog-conversations', 20),
                              ('groundhog-store-conversations', 120)]:
            props = r._properties(self.tests[name])
            self.assertEqual(props['TIMEOUT'], timeout)
            self.assertEqual(props['FIXTURES_REQUIRED'], ['groundhog-test-homes'])
            self.assertIn('NOSTRC_TEST_ISOLATION=1', props['ENVIRONMENT'])
            self.assertIn('GSETTINGS_BACKEND=memory', props['ENVIRONMENT'])
            self.assertTrue(any(v.startswith('G_DEBUG=fatal-criticals') for v in props['ENVIRONMENT']))
            self.assertNotIn('SKIP_RETURN_CODE', props)
        homes = self.tests['groundhog-test-homes']
        self.assertEqual(homes['command'], ['/usr/bin/cmake', '-P',
                         '{build}/gnome/groundhog/groundhog-test-homes.cmake'])
        self.assertEqual(self.tests['groundhog-test-gst-registry']['command'],
                         ['/usr/bin/gst-inspect-1.0', 'coreelements'])
        for name in ('groundhog-test-homes', 'groundhog-test-gst-registry'):
            self.assertEqual(self.descriptors[name]['expected'], ['fixture'])
            self.assertEqual(self.descriptors[name]['owner'], ['testing.framework'])
        self.assertEqual(self.descriptors['groundhog-test-homes']['inputs'], [
            '{build}/gnome/groundhog/groundhog-test-homes.cmake',
            '{source}/cmake/NostrcTestIsolation.cmake'])
        for ident in CHECKS:
            self.assert_input_covered('{source}/cmake/NostrcTestIsolation.cmake',
                                      self.policy['checks'][ident])

    def test_missing_fixture_or_wrong_executable_refuses(self):
        tests = copy.deepcopy(self.tests)
        del tests['groundhog-test-homes']
        del tests['groundhog-test-gst-registry']
        with self.assertRaisesRegex(r.RealizationError, 'missing setup'):
            r._fixture_closure(['groundhog-store'], tests, self.descriptors)
        bad = copy.deepcopy(self.tests['groundhog-store'])
        bad['command'] = ['{build}/unmapped-store']
        with self.assertRaisesRegex(r.RealizationError, 'lacks explicit inputs/targets'):
            r._mapped_targets(bad, self.descriptors[bad['name']], self.real)

    def test_removing_semantic_prerequisites_loses_real_inputs(self):
        for ident, name, path in [
                ('groundhog.model_store', 'groundhog-store', 'gnome/groundhog/src/store/gh-store.c'),
                ('groundhog.model_store_conversations', 'groundhog-store-conversations',
                 'gnome/groundhog/src/store/gh-store-conversations.c')]:
            check = copy.deepcopy(self.policy['checks'][ident])
            check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
            with self.assertRaises(AssertionError):
                self.assert_input_covered('{source}/' + path, check)
        self.assert_input_covered('{source}/nostr-gtk/src/gn-nostr-reference.c',
                                  self.policy['checks']['groundhog.model_conversations'])
        self.policy['nodes']['groundhog.conversations']['depends_on'].remove('nostr.gtk.portable')
        with self.assertRaises(AssertionError):
            self.assert_input_covered('{source}/nostr-gtk/src/gn-nostr-reference.c',
                                      self.policy['checks']['groundhog.model_conversations'])

    def test_existing_workflow_authorizes_names_not_full_application_or_mdk(self):
        script = step_script((ROOT / '.github/workflows/groundhog-ci.yml').read_text(),
                             'Run all Groundhog tests under a controlled display')
        include = re.search(r"-R\s+'([^']+)'", script).group(1)
        exclude = re.search(r"-E\s+'([^']+)'", script).group(1)
        for name in CHECKS.values():
            self.assertRegex(name, include)
            self.assertNotRegex(name, exclude)
        self.assertEqual(self.policy['profiles'][PROFILE], self.export['profile'])
        ordinary = [name for check in self.policy['checks'].values()
                    for name in check.get('ctest_tests', {}).get(PROFILE, [])]
        self.assertNotIn('groundhog-store-sqlite', ordinary)
        self.assertNotIn('groundhog-store-marmot', ordinary)
        # Exact family enrollment must not undo the portable/UI no-MDK split.
        plan = s.make_plan(self.policy, [change('nostr-gtk/data/nostr-gtk.gresource.xml')], head='a' * 40)
        self.assertFalse(any('mdk' in lane for lane in plan['expected_lane_ids']))
        self.assertTrue(set(CHECKS).isdisjoint(plan['selected_checks']))

    def test_reviewed_subset_expands_selected_closure_not_omissions(self):
        import test_selection_subset as subset
        path = 'tests/selection/fixtures/headless_model_subset_renewal_20261022.json'
        renewal = json.loads((ROOT / path).read_text())
        contract = subset.contracts(self.policy)[PROFILE]
        family = sorted(CHECKS.values())
        self.assertEqual(renewal['added_primary'], family)
        authored = sorted(name for check in self.policy['checks'].values()
                          if check.get('kind') == 'ctest'
                          for name in check.get('ctest_tests', {}).get(PROFILE, []))
        self.assertEqual(contract['primary_registrations'], authored)
        previous = copy.deepcopy(contract)
        for key in ('primary_registrations', 'required_closure'):
            self.assertEqual(contract[key], sorted(renewal['previous'][key] + family))
            self.assertEqual(contract[key], renewal['renewed'][key])
            previous[key] = renewal['previous'][key]
        # Historical authority/source/profile/candidate hashes remain intact.
        self.assertEqual(s.digest(previous), renewal['previous_record_hash'])
        self.assertEqual(contract['registrations'], list(subset.REGISTRATIONS))
        self.assertEqual(len(contract['registrations']), 33)
        self.assertEqual(contract['candidate_contract_hash'], renewal['candidate_contract_hash'])
        executions = r._fixture_closure(family, self.tests, self.descriptors)
        self.assertTrue(set(executions) <= set(contract['required_closure']))
        self.assertFalse(set(executions) <= set(previous['required_closure']))
        self.assertTrue(set(executions).isdisjoint(contract['registrations']))
        self.assertEqual(renewal['build']['exit_code'], 0)
        self.assertEqual(renewal['test']['exit_code'], 0)
        self.assertEqual(renewal['headless_environment'], {
            'DISPLAY': None, 'WAYLAND_DISPLAY': None, 'DBUS_SESSION_BUS_ADDRESS': None})
        self.assertFalse(any(renewal['limits'].values()))

    def test_test_sources_are_selection_contract_inputs(self):
        inputs = self.policy['checks']['selection.contract']['runtime_inputs']
        for path in (FIXTURE, 'tests/selection/test_headless_model_membership.py',
                     'tests/selection/fixtures/headless_model_subset_renewal_20261022.json'):
            self.assertIn('{source}/' + path, inputs)


if __name__ == '__main__':
    unittest.main()
