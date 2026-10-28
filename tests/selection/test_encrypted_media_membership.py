"""Bounded ordinary encrypted-media membership; no profile execution acceptance."""
import copy
import hashlib
import json
import unittest

from selector_support import ROOT, change, s
from realization_support import r
from encrypted_media_projection import (
    ADDED, BASE, EXPORT, FAMILY, RENEWAL, before_encrypted_media_family,
    verify_historical_media_fixtures,
)

PROFILE = 'groundhog.linux'
CHECKS = {
    'groundhog.blossom': 'groundhog-blossom',
    'groundhog.media': 'groundhog-media',
    'groundhog.media_cache_binding': 'groundhog-media-cache-binding',
    'groundhog.media_purge': 'groundhog-media-purge',
}
SCHEMA = '{source}/gnome/groundhog/data/org.nostr.Groundhog.gschema.xml'
SCHEMA_TESTS = {'groundhog-blossom', 'groundhog-media-cache-binding'}


class _MembershipFixture:
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



class EncryptedMediaMembership(_MembershipFixture, unittest.TestCase):
    def test_exact_ordinary_checks_and_explicit_build_prerequisites(self):
        self.assertEqual(sorted(CHECKS.values()), FAMILY)
        for ident, name in CHECKS.items():
            check = self.p['checks'][ident]
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            expected = ['test-' + name]
            if name in SCHEMA_TESTS:
                expected.insert(0, 'groundhog-test-schemas')
            self.assertEqual(check['build_targets'], expected)
            self.assertEqual(check['runtime_inputs'], [])
            self.assertEqual((check['kind'], check['cost'], check['requirement'],
                              check['expected_outcome']), ('ctest', 'ordinary', 'blocking', 'pass'))
            self.assertEqual([k for k, v in self.p['checks'].items()
                              if name in v.get('ctest_tests', {}).get(PROFILE, [])], [ident])
            self.assertEqual(self.ds[name]['requires'], [])
            self.assertEqual(self.ds[name]['owner'], ['groundhog.unclassified'])
            self.assertEqual(self.ds[name]['inputs'], [SCHEMA] if name in SCHEMA_TESTS else [])
        self.assertFalse(self.p['enforcement_ready'])

    def test_schema_fatal_criticals_isolation_and_timeout_guards(self):
        for name in FAMILY:
            props = r._properties(self.tests[name])
            self.assertEqual(props['TIMEOUT'], 120 if name == 'groundhog-blossom' else 60)
            self.assertNotIn('SKIP_RETURN_CODE', props)
            self.assertEqual(props['FIXTURES_REQUIRED'], ['groundhog-test-homes'])
            env = props['ENVIRONMENT']
            for item in ('G_DEBUG=fatal-criticals', 'GSETTINGS_BACKEND=memory',
                         'NOSTRC_TEST_ISOLATION=1'):
                self.assertIn(item, env)
            self.assertIn('HOME={build}/gnome/groundhog/test-homes/' + name + '/home', env)
            self.assertIn('GST_REGISTRY={build}/gnome/groundhog/gst-registry.bin', env)
            self.assertEqual(any(v.startswith('GSETTINGS_SCHEMA_DIR=') for v in env),
                             name in SCHEMA_TESTS)
            self.assertFalse(any(v.startswith(('DISPLAY=', 'WAYLAND_DISPLAY=',
                                              'DBUS_SESSION_BUS_ADDRESS=')) for v in env))

    def test_source_header_schema_and_fixture_changes_select_family(self):
        for path in ('gnome/groundhog/src/media/gh-attachment.c',
                     'gnome/groundhog/src/media/gh-attachment.h',
                     'gnome/groundhog/tests/media/blossom-fixture.c',
                     'gnome/groundhog/tests/privacy/canary-scan.c'):
            plan = s.make_plan(self.p, [change(path)], head=BASE, profiles=[PROFILE])
            self.assertTrue(set(CHECKS) <= set(plan['selected_checks']), path)
        schema_plan = s.make_plan(self.p, [change(SCHEMA.removeprefix('{source}/'))],
                                  head=BASE, profiles=[PROFILE])
        self.assertTrue({'groundhog.blossom', 'groundhog.media_cache_binding'} <=
                        set(schema_plan['selected_checks']))
        ordinary = {name for ident in CHECKS
                    for name in self.p['checks'][ident]['ctest_tests'][PROFILE]}
        self.assertTrue(ordinary.isdisjoint({'groundhog-attachments', 'groundhog-attachments-view',
                                            'groundhog-voice', 'groundhog-dm-send'}))

    def test_missing_fixture_wrong_artifact_and_semantic_edge_refuse(self):
        tests = copy.deepcopy(self.tests)
        del tests['groundhog-test-homes']
        del tests['groundhog-test-gst-registry']
        with self.assertRaisesRegex(r.RealizationError, 'missing setup'):
            r._fixture_closure(FAMILY, tests, self.ds)
        bad = copy.deepcopy(self.tests['groundhog-media'])
        bad['command'] = ['{build}/not-the-media-artifact']
        with self.assertRaisesRegex(r.RealizationError, 'lacks explicit inputs/targets'):
            r._mapped_targets(bad, self.ds['groundhog-media'], self.real)
        check = copy.deepcopy(self.p['checks']['groundhog.media'])
        check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
        with self.assertRaises(AssertionError):
            self.assert_covered('{source}/gnome/groundhog/src/media/gh-attachment.c', check)
        check = copy.deepcopy(self.p['checks']['groundhog.blossom'])
        check['nodes'] = ['execution.groundhog', 'groundhog.unclassified']
        with self.assertRaises(AssertionError):
            self.assert_covered(SCHEMA, check)



class SourceClosure(_MembershipFixture, unittest.TestCase):
    """Retained native export/input matrix; direct/manual discovery only."""
    def test_actual_command_artifact_compiled_sources_and_fixture_closure(self):
        for ident, name in CHECKS.items():
            ids = r._target_closure(r._mapped_targets(self.tests[name], self.ds[name], self.real),
                                    self.real)
            targets = sorted(self.real['targets'][key]['name'] for key in ids)
            sources = sorted({entry['path'] for key in ids for entry in
                              self.real['targets'][key]['sources']
                              if entry['compiled'] and not entry['generated']})
            witness = self.export['primary'][name]
            self.assertEqual(targets, witness['closure_targets'])
            self.assertEqual(len(targets), 26 if name == 'groundhog-media' else 27)
            self.assertEqual(sources, witness['source_inputs'])
            self.assertEqual(len(sources), {'groundhog-media': 199, 'groundhog-blossom': 202,
                                          'groundhog-media-cache-binding': 201,
                                          'groundhog-media-purge': 202}[name])
            self.assertTrue(set(targets).isdisjoint(
                {'groundhog', 'groundhog-ui', 'marmot', 'nostr_gtk', 'groundhog-mls-media'}))
            for path in sources + self.ds[name]['inputs']:
                self.assert_covered(path, self.p['checks'][ident])
            closure = r._fixture_closure([name], self.tests, self.ds)
            self.assertEqual(closure, sorted([name, 'groundhog-test-homes',
                                              'groundhog-test-gst-registry']))
            self.assertEqual(closure, witness['closure_names'])
        self.assertIn('groundhog-store-conversations',
                      self.export['primary']['groundhog-media-purge']['closure_targets'])





if __name__ == '__main__':
    unittest.main()
