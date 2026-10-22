"""Six safe ordinary crypto registrations; no vector fallback/profile credit."""
import copy
import hashlib
import json
import unittest

from selector_support import ROOT, change, s
from grotto_crypto_projection import (BASE, CHECKS, EXPORT, FAMILY, PROFILE, RENEWAL,
                                      before_grotto_crypto_family,
                                      grotto_crypto_current_workflow_checks,
                                      renewal_witness)
from realization_support import r


class GrottoCryptoMembership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.witness = renewal_witness()
        self.export = json.loads((ROOT / EXPORT).read_text())
        self.real = self.export['realization']
        self.tests, self.descriptors = r._test_maps(self.real)

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

    def test_six_only_arrays_and_strict_full_record_projection(self):
        contract = self.policy['diagnostic_nonmember_subsets'][PROFILE]
        before = before_grotto_crypto_family(contract)
        authored = sorted(n for c in self.policy['checks'].values()
                          for n in c.get('ctest_tests', {}).get(PROFILE, []))
        self.assertEqual(contract['primary_registrations'], authored)
        self.assertEqual((len(authored), len(contract['required_closure'])), (64, 67))
        self.assertEqual((len(before['primary_registrations']), len(before['required_closure'])), (58, 61))
        self.assertEqual(len(contract['registrations']), 33)
        self.assertEqual(len(contract['review']['source_inputs']), 193)
        self.assertTrue(set(FAMILY).isdisjoint(contract['registrations']))
        self.assertEqual(s.digest(before), self.witness['previous_record_hash'])
        for key in contract.keys() - {'primary_registrations', 'required_closure'}:
            self.assertEqual(contract[key], before[key], key)
        for key in ('candidate_contract_hash', 'profile_hash', 'review', 'registration_universe',
                    'toolchain_contract_hash', 'registrations'):
            bad = copy.deepcopy(contract)
            bad[key] = 'changed-authority'
            with self.assertRaisesRegex(s.ContractError, 'non-selection authority'):
                before_grotto_crypto_family(bad)
        for key in ('primary_registrations', 'required_closure'):
            for name in FAMILY:
                bad = copy.deepcopy(contract)
                bad[key].remove(name)
                with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
                    before_grotto_crypto_family(bad)
            for name in ('test_interop', 'test_nip44_vectors', 'unknown-registration'):
                bad = copy.deepcopy(contract)
                bad[key] = sorted(bad[key] + [name])
                with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
                    before_grotto_crypto_family(bad)
        with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
            before_grotto_crypto_family(before)

    def test_workflow_projection_requires_exact_prior_check_list(self):
        previous = copy.deepcopy(self.policy)
        for ident in CHECKS:
            del previous['checks'][ident]
        contract = self.policy['diagnostic_nonmember_subsets'][PROFILE]
        previous['diagnostic_nonmember_subsets'][PROFILE] = before_grotto_crypto_family(contract)
        path = '.github/workflows/grotto-ci.yml'
        old = s.make_plan(previous, [change(path)], head=BASE, profiles=[PROFILE])['selected_checks']
        current = s.make_plan(self.policy, [change(path)], head=BASE, profiles=[PROFILE])['selected_checks']
        self.assertEqual(grotto_crypto_current_workflow_checks(old, contract), current)
        with self.assertRaisesRegex(s.ContractError, 'historical workflow checks changed'):
            grotto_crypto_current_workflow_checks(old + ['unexpected.check'], contract)

    def test_vector_and_interop_remain_unclaimed_without_fallback_credit(self):
        contract = self.policy['diagnostic_nonmember_subsets'][PROFILE]
        for name in ('test_nip44_vectors', 'test_interop'):
            self.assertFalse(any(name in c.get('ctest_tests', {}).get(PROFILE, [])
                                 for c in self.policy['checks'].values()))
            self.assertNotIn(name, contract['primary_registrations'])
            self.assertNotIn(name, contract['required_closure'])
            self.assertIn(name, self.witness['deferred'])
        vector = self.export['excluded']['test_nip44_vectors']
        self.assertEqual(vector['status'], 'unclaimed_canonical_json_and_fallback_gap')
        self.assertEqual(vector['descriptor']['inputs'], [])
        self.assertEqual(self.witness['deferred']['test_nip44_vectors']['required_runtime_inputs'],
                         ['{source}/nips/nip44/tests/nip44.vectors.json'])
        self.assertEqual(self.export['excluded']['test_interop']['status'],
                         'unclaimed_pending_independent_semantics')

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

    def test_history_sources_and_other_authorities_are_preserved(self):
        self.assertEqual(self.export['source_identity'], {'kind': 'committed', 'sha': BASE})
        self.assertEqual(self.export['realization_sha256'], self.witness['fresh_realization_sha256'])
        self.assertEqual(hashlib.sha256((ROOT / EXPORT).read_bytes()).hexdigest(),
                         self.witness['export_sha256'])
        for key, digest in self.witness['unchanged_contract_hashes'].items():
            self.assertEqual(s.digest(self.policy[key]), digest, key)
        self.assertEqual(s.digest(self.policy['diagnostic_nonmember_subsets']['groundhog.linux']),
                         self.witness['unchanged_groundhog_subset_hash'])
        self.assertEqual(self.policy['profiles'][PROFILE], self.export['profile'])
        for path, digest in self.witness['historical_fixture_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest, path)
        self.assertEqual(hashlib.sha256((ROOT / 'tests/selection/golden/selection.json').read_bytes()).hexdigest(),
                         self.witness['historical_golden_sha256'])
        for path, digest in self.export['reviewed_source_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest, path)

    def test_physical_evidence_is_raw_headless_diagnostic_not_acceptance(self):
        proof = self.witness['physical']
        self.assertEqual(proof['source_identity'], self.export['source_identity'])
        self.assertEqual(proof['required_capabilities'], [])
        self.assertEqual(proof['profile_environment'], self.export['profile']['environment'])
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


if __name__ == '__main__':
    unittest.main()
