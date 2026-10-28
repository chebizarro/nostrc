"""Six safe ordinary crypto registrations; no vector fallback/profile credit."""
import copy
import json
import unittest

from selector_support import ROOT, change, s
from grotto_crypto_projection import BASE, CHECKS, EXPORT, FAMILY, PROFILE, renewal_witness
from realization_support import r


class _MembershipFixture:
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.witness = renewal_witness()
        self.export = json.loads((ROOT / EXPORT).read_text())
        self.real = self.export['realization']
        self.tests, self.descriptors = r._test_maps(self.real)


class GrottoCryptoMembership(_MembershipFixture, unittest.TestCase):
    def test_exact_single_ownership_only_ordinary_grotto(self):
        self.assertEqual(sorted(self.export['primary']), FAMILY)
        for ident, name in CHECKS.items():
            check = self.policy['checks'][ident]
            self.assertEqual(check, self.witness['checks'][ident])
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['build_targets'], [name])
            nip = self.export['primary'][name]['nip']
            self.assertEqual(check['nodes'], ['execution.grotto', nip, nip + '.test_inputs'])
            self.assertEqual((check['kind'], check['cost'], check['requirement'],
                              check['expected_outcome']), ('ctest', 'ordinary', 'blocking', 'pass'))
            self.assertEqual([k for k, c in self.policy['checks'].items()
                              if name in c.get('ctest_tests', {}).get(PROFILE, [])], [ident])
            self.assertEqual(check['runtime_inputs'], [])
        self.assertFalse(self.policy['enforcement_ready'])

    def test_wrong_executable_or_missing_target_refuses(self):
        name = FAMILY[0]
        bad = copy.deepcopy(self.tests[name])
        bad['command'] = ['{build}/not-the-crypto-executable']
        with self.assertRaisesRegex(r.RealizationError, 'lacks explicit inputs/targets'):
            r._mapped_targets(bad, self.descriptors[name], self.real)
        real = copy.deepcopy(self.real)
        for key in list(real['targets']):
            if real['targets'][key]['name'] == name:
                del real['targets'][key]
        with self.assertRaises(r.RealizationError):
            r._mapped_targets(self.tests[name], self.descriptors[name], real)

    def test_semantic_selection_uses_existing_nip_nodes(self):
        for nip in ('nip04', 'nip44'):
            expected = {ident for ident, name in CHECKS.items() if self.export['primary'][name]['nip'] == nip}
            for relative in (('nips/nip04/src/nip04.c', 'nips/nip04/tests/test_roundtrip.c') if nip == 'nip04'
                             else ('nips/nip44/src/core/nip44.c', 'nips/nip44/tests/test_nip44_roundtrip.c')):
                self.assertTrue((ROOT / relative).is_file(), relative)
                plan = s.make_plan(self.policy, [change(relative)], head=BASE, profiles=[PROFILE])
                self.assertEqual(set(CHECKS) & set(plan['selected_checks']), expected)
                self.assertFalse(any(self.policy['checks'][key]['cost'] == 'live_reference'
                                     for key in plan['selected_checks']))
            covered = r._impact_prerequisites(self.policy, ['execution.grotto', nip + '.test_inputs'])
            self.assertNotIn(nip, covered)

    def test_check_local_vectors_cannot_supply_missing_canonical_authority(self):
        name = 'test_nip44_vectors'
        vector = self.export['excluded'][name]
        check = copy.deepcopy(self.policy['checks']['grotto.nip44_roundtrip'])
        check['ctest_tests'] = {PROFILE: [name]}
        check['runtime_inputs'] = self.witness['deferred'][name]['required_runtime_inputs']
        real = {'repo_root': str(ROOT), 'build_dir': str(ROOT / '_not_executed'),
                'profile_id': PROFILE}
        _, gaps = r._ctest_runtime_constraints({'checks': {'grotto.nip44_vectors': check}}, real,
                                               {name: vector['test']}, {name: vector['descriptor']})
        self.assertEqual(len(gaps), 1)
        self.assertEqual(gaps[0]['kind'], 'ctest_runtime_input_consistency')
        self.assertIn('absent from authoritative descriptor/fixture closure', gaps[0]['reason'])

    def test_physical_evidence_is_raw_headless_diagnostic_not_acceptance(self):
        proof = self.witness['physical']
        self.assertEqual(proof['source_identity'], self.export['source_identity'])
        self.assertEqual(proof['required_capabilities'], [])
        self.assertEqual(proof['build']['exit_code'], 0)
        self.assertEqual(proof['test']['exit_code'], 0)
        self.assertEqual(proof['test']['executed'], FAMILY)
        self.assertEqual(proof['test']['skips'], 0)
        self.assertEqual(proof['build']['argv'][3:5], ['--parallel', '2'])
        self.assertEqual(proof['test']['argv'][proof['test']['argv'].index('--parallel') + 1], '1')
        self.assertFalse(any(proof['headless_environment'].values()))
        self.assertIn('raw diagnostic', proof['execution'])
        self.assertFalse(proof['full_profile_execution'])
        self.assertFalse(any(self.witness['limits'].values()))


class SourceClosure(_MembershipFixture, unittest.TestCase):
    """Retained native export/input matrix; direct/manual discovery only."""
    def test_real_executables_and_transitive_compiled_input_closure(self):
        for ident, name in CHECKS.items():
            witness = self.export['primary'][name]
            nip = witness['nip']
            descriptor = self.descriptors[name]
            self.assertEqual(descriptor['owner'], [nip, nip + '.test_inputs'])
            self.assertEqual(descriptor['inputs'], [])
            self.assertEqual(descriptor['requires'], [])
            self.assertEqual(self.tests[name]['command'], ['{build}/nips/' + nip + '/' + name])
            ids = r._target_closure(r._mapped_targets(self.tests[name], descriptor, self.real), self.real)
            targets = sorted(self.real['targets'][key]['name'] for key in ids)
            library = 'nip04' if nip == 'nip04' else 'nostr_nip44_core'
            self.assertEqual(targets, sorted([library, 'nostr', 'nostrdb', 'nostrgo', name]))
            self.assertEqual(targets, witness['closure_targets'])
            sources = sorted({v['path'] for key in ids for v in self.real['targets'][key]['sources']
                              if v['compiled'] and not v['generated']})
            self.assertEqual(sources, witness['source_inputs'])
            self.assertEqual(len(sources), 88 if nip == 'nip04' else 93)
            covered = r._impact_prerequisites(self.policy, self.policy['checks'][ident]['nodes'])
            for path in sources:
                relative = path.removeprefix('{source}/')
                self.assertTrue((ROOT / relative).is_file(), relative)
                classified = s.classify(self.policy, relative)
                self.assertFalse(classified.get('unclassified'), relative)
                if classified['effect'] != 'global':
                    self.assertEqual(classified['effect'], 'impact', relative)
                    self.assertTrue(set(classified['nodes']) <= covered, relative)
            self.assertEqual(r._fixture_closure([name], self.tests, self.descriptors), [name])
            self.assertEqual(witness['closure_names'], [name])
            props = r._properties(self.tests[name])
            self.assertEqual(props, {'LABELS': ['selection:' + nip + '.test_inputs'],
                                    'WORKING_DIRECTORY': '{build}/nips/' + nip})


if __name__ == '__main__':
    unittest.main()
