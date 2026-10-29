"""Exact offline relay/network transport family; no full-profile acceptance."""
import copy
import json
import unittest

from headless_transport_projection import BASE, EXPORT, FAMILY, RENEWAL
from selector_support import ROOT, change, s
from realization_support import r

PROFILE = 'groundhog.linux'
CHECKS = {
    'groundhog.transport_net': 'groundhog-net',
    'groundhog.transport_publish': 'groundhog-relay-publish',
    'groundhog.transport_publish_wire': 'groundhog-relay-publish-wire',
    'groundhog.transport_scope': 'groundhog-relay',
    'groundhog.transport_tls': 'groundhog-tls-resumption',
    'groundhog.transport_wire': 'groundhog-relay-wire',
}


class _MembershipFixture:
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.export = json.loads((ROOT / EXPORT).read_text())
        self.renewal = json.loads((ROOT / RENEWAL).read_text())
        self.real = self.export['realization']
        self.tests, self.descriptors = r._test_maps(self.real)

    def assert_input_covered(self, path, check):
        relative = path.removeprefix('{source}/')
        self.assertTrue((ROOT / relative).is_file(), relative)
        classified = s.classify(self.policy, relative)
        self.assertFalse(classified.get('unclassified'), relative)
        if classified['effect'] != 'global':
            self.assertEqual(classified['effect'], 'impact', relative)
            covered = r._impact_prerequisites(self.policy, check['nodes'])
            self.assertTrue(set(classified['nodes']) <= covered, relative)
            plan = s.make_plan(self.policy, [change(relative)], head=BASE, profiles=[PROFILE])
            self.assertIn(next(k for k, v in CHECKS.items()
                               if self.policy['checks'][k] == check), plan['selected_checks'])

    def assert_historical_nodes(self, nodes):
        reviewed = {
            'grelay': (
                ['nostr.gobject', 'relayd'], ['nostr.gobject'], ['apps/grelay'],
                'grelay and grelay_ingress_security link relay_security, whose relay_policy.c, rate_limit.c and relay_ingress.c sources and public headers belong to apps/relayd (apps/grelay/CMakeLists.txt; apps/grelay/tests/CMakeLists.txt).'),
            'relayd': (
                ['libnostr', 'libjson'], ['libnostr'], ['apps/relayd'],
                'nostr-relay-server PRIVATE-links nostr_json; nostrc-relayd and the registered ingress/REQ/storage consumers also link nostr_json (apps/relayd/CMakeLists.txt; apps/relayd/tests/CMakeLists.txt). Its libjson source/header inputs must reach relay consumers.'),
            'shared.tests': (
                ['libnostr', 'libjson'], ['libnostr'], ['tests'],
                'Root shared tests, including test_connection_recv_drain, explicitly link nostr_json (tests/CMakeLists.txt); libjson/src/json.c and its public header are build inputs, not optional external JSON providers.'),
        }
        previous = copy.deepcopy(nodes)
        for ident, (current, old, paths, rationale) in reviewed.items():
            self.assertEqual(nodes[ident], {
                'id': ident, 'depends_on': current, 'owner_paths': paths, 'rationale': rationale,
            })
            previous[ident]['depends_on'] = old
            previous[ident]['rationale'] = (
                'Prerequisite-to-consumer impact; realized target/runtime input audit must confirm complete closure.')
        self.assertEqual(s.digest(previous), self.renewal['unchanged_contract_hashes']['nodes'])


class HeadlessTransportMembership(_MembershipFixture, unittest.TestCase):
    def test_exact_ordinary_family_excludes_other_executions(self):
        self.assertEqual(sorted(CHECKS.values()), FAMILY)
        ordinary = {name for ident in CHECKS
                    for name in self.policy['checks'][ident]['ctest_tests'][PROFILE]}
        excluded = {'groundhog-relayd-publish', 'groundhog-dm-send', 'groundhog-outbox',
                    'groundhog-nip29-service', 'groundhog-nip46-wire', 'groundhog-account-ui'}
        self.assertTrue(ordinary.isdisjoint(excluded))
        for ident, name in CHECKS.items():
            check = self.policy['checks'][ident]
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            self.assertEqual(check['build_targets'], ['test-' + name])
            self.assertEqual((check['kind'], check['cost'], check['requirement'],
                              check['expected_outcome']), ('ctest', 'ordinary', 'blocking', 'pass'))
            self.assertEqual(check['runtime_inputs'], [])
            self.assertEqual([k for k, v in self.policy['checks'].items()
                              if name in v.get('ctest_tests', {}).get(PROFILE, [])], [ident])
            self.assertEqual(self.descriptors[name]['requires'], [])
            self.assertEqual(self.descriptors[name]['owner'], ['groundhog.unclassified'])
        self.assertFalse(self.policy['enforcement_ready'])

    def test_isolation_schema_timeout_and_skip_guards_unchanged(self):
        timeouts = {'groundhog-net': 120, 'groundhog-tls-resumption': 60,
                    'groundhog-relay-wire': 60, 'groundhog-relay-publish-wire': 90}
        for name in FAMILY:
            props = r._properties(self.tests[name])
            self.assertEqual(props['TIMEOUT'], timeouts.get(name, 20))
            self.assertEqual(props['FIXTURES_REQUIRED'], ['groundhog-test-homes'])
            self.assertIn('NOSTRC_TEST_ISOLATION=1', props['ENVIRONMENT'])
            self.assertIn('GSETTINGS_BACKEND=memory', props['ENVIRONMENT'])
            if name == 'groundhog-tls-resumption':
                self.assertEqual(props['SKIP_RETURN_CODE'], 77)
            else:
                self.assertNotIn('SKIP_RETURN_CODE', props)
            if name in ('groundhog-net', 'groundhog-tls-resumption'):
                self.assertIn('G_DEBUG=fatal-criticals', props['ENVIRONMENT'])
                self.assertIn('groundhog-test-schemas', self.descriptors[name]['targets'])
                self.assertEqual(self.descriptors[name]['inputs'],
                                 ['{source}/gnome/groundhog/data/org.nostr.Groundhog.gschema.xml'])
            else:
                self.assertFalse(any(v.startswith('G_DEBUG=') for v in props['ENVIRONMENT']))
            self.assertFalse(any(v.startswith(('NOSTRC_RELAYD=', 'GH_TEST_INTEROP_RELAY_URL='))
                                 for v in props['ENVIRONMENT']))

    def test_missing_fixture_wrong_artifact_and_catchall_only_refuse(self):
        tests = copy.deepcopy(self.tests)
        del tests['groundhog-test-homes']
        del tests['groundhog-test-gst-registry']
        with self.assertRaisesRegex(r.RealizationError, 'missing setup'):
            r._fixture_closure(FAMILY, tests, self.descriptors)
        bad = copy.deepcopy(self.tests['groundhog-relay'])
        bad['command'] = ['{build}/not-the-relay-artifact']
        with self.assertRaisesRegex(r.RealizationError, 'lacks explicit inputs/targets'):
            r._mapped_targets(bad, self.descriptors['groundhog-relay'], self.real)
        check = copy.deepcopy(self.policy['checks']['groundhog.transport_scope'])
        check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
        with self.assertRaises(AssertionError):
            self.assert_input_covered('{source}/gnome/groundhog/src/relay/gh-relay-scope.c', check)

    def test_physical_evidence_is_capability_honest_diagnostic_only(self):
        proof = self.renewal['physical']
        self.assertEqual(proof['source_identity'], self.export['source_identity'])
        self.assertEqual(proof['required_capabilities'], [])
        self.assertFalse(any(proof['headless_environment'].values()))
        self.assertEqual(proof['build']['exit_code'], 0)
        self.assertEqual(proof['test']['exit_code'], 0)
        self.assertEqual(proof['test']['skips'], 0)
        self.assertEqual(sorted(proof['test']['executed']), sorted(FAMILY +
                         ['groundhog-test-gst-registry', 'groundhog-test-homes']))
        self.assertIn('raw diagnostic', proof['execution'])
        self.assertFalse(any(self.renewal['limits'].values()))

    def test_new_files_are_immediately_contract_inputs(self):
        for path in (EXPORT, RENEWAL, 'tests/selection/headless_transport_projection.py',
                     'tests/selection/test_headless_transport_membership.py'):
            self.assertIn('{source}/' + path, self.policy['checks']['selection.contract']['runtime_inputs'])


class SourceClosure(_MembershipFixture, unittest.TestCase):
    """Retained native export/input matrix; direct/manual discovery only."""
    def test_compiled_descriptor_runtime_and_fixture_closures(self):
        counts = [22, 18, 24, 18, 22, 21]
        for (ident, name), count in zip(CHECKS.items(), counts):
            check = self.policy['checks'][ident]
            desc = self.descriptors[name]
            ids = r._target_closure(r._mapped_targets(self.tests[name], desc, self.real), self.real)
            targets = sorted(self.real['targets'][key]['name'] for key in ids)
            witness = self.export['primary'][name]
            self.assertEqual(targets, witness['closure_targets'])
            self.assertEqual(len(targets), count)
            self.assertTrue(set(targets).isdisjoint({'groundhog', 'groundhog-ui', 'marmot', 'nostr_gtk'}))
            sources = sorted({entry['path'] for key in ids for entry in self.real['targets'][key]['sources']
                              if entry['compiled'] and not entry['generated']})
            self.assertEqual(sources, witness['source_inputs'])
            for path in sources + desc['inputs']:
                self.assert_input_covered(path, check)
            closure = r._fixture_closure([name], self.tests, self.descriptors)
            self.assertEqual(closure, sorted([name, 'groundhog-test-homes', 'groundhog-test-gst-registry']))
            self.assertEqual(closure, witness['closure_names'])
        # The daemon is a physical build prerequisite, not selection of its alias test.
        self.assertIn('nostrc-relayd', self.export['primary']['groundhog-relay-publish-wire']['closure_targets'])


if __name__ == '__main__':
    unittest.main()
