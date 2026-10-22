"""Exact headless NIP-49 root-export enrollment, not full Grotto acceptance."""
import copy
import hashlib
import json
import unittest

from grotto_nip49_projection import (BASE, EXPORT, FAMILY, PROFILE, RENEWAL,
                                     before_grotto_nip49_family)
from realization_support import r
from selector_support import ROOT, change, s

CHECKS = {'grotto.' + name.removeprefix('test_'): name for name in FAMILY}
SPEC = '{source}/nips/nip49/SPEC_SOURCE'


class GrottoNip49Membership(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.export = json.loads((ROOT / EXPORT).read_text())
        self.renewal = json.loads((ROOT / RENEWAL).read_text())
        self.real = self.export['realization']
        self.tests, self.descriptors = r._test_maps(self.real)

    def test_exact_single_ownership_and_no_other_profile_enrollment(self):
        self.assertEqual(sorted(self.export['primary']), FAMILY)
        for ident, name in CHECKS.items():
            check = self.policy['checks'][ident]
            self.assertEqual(check['ctest_tests'], {PROFILE: [name]})
            self.assertEqual(check['profiles'], [PROFILE])
            self.assertEqual(check['build_targets'], [name])
            self.assertEqual(check['nodes'], ['execution.grotto', 'nip49', 'nip49.test_inputs'])
            self.assertEqual((check['kind'], check['cost'], check['requirement'],
                              check['expected_outcome']), ('ctest', 'ordinary', 'blocking', 'pass'))
            self.assertEqual([k for k, v in self.policy['checks'].items()
                              if name in v.get('ctest_tests', {}).get(PROFILE, [])], [ident])
        self.assertFalse(self.policy['enforcement_ready'])

    def test_real_executable_compiled_transitive_and_fixture_closure(self):
        for ident, name in CHECKS.items():
            desc = self.descriptors[name]
            self.assertEqual(desc['owner'], ['nip49', 'nip49.test_inputs'])
            self.assertEqual(desc['requires'], [])
            self.assertEqual(self.tests[name]['command'], ['{build}/nips/nip49/' + name])
            ids = r._target_closure(r._mapped_targets(self.tests[name], desc, self.real), self.real)
            targets = sorted(self.real['targets'][key]['name'] for key in ids)
            self.assertEqual(targets, sorted(['nip19', 'nostr', 'nostr_nip49_core', 'nostrdb', 'nostrgo', name]))
            witness = self.export['primary'][name]
            self.assertEqual(targets, witness['closure_targets'])
            sources = sorted({v['path'] for key in ids for v in self.real['targets'][key]['sources']
                              if v['compiled'] and not v['generated']})
            self.assertEqual(sources, witness['source_inputs'])
            self.assertEqual(len(sources), 96)
            for path in sources + desc['inputs']:
                relative = path.removeprefix('{source}/')
                self.assertTrue((ROOT / relative).is_file(), relative)
                classified = s.classify(self.policy, relative)
                self.assertFalse(classified.get('unclassified'), relative)
                if classified['effect'] != 'global':
                    self.assertEqual(classified['effect'], 'impact', relative)
                    covered = r._impact_prerequisites(self.policy, self.policy['checks'][ident]['nodes'])
                    self.assertTrue(set(classified['nodes']) <= covered, relative)
            self.assertEqual(r._fixture_closure([name], self.tests, self.descriptors), [name])
            self.assertEqual(witness['closure_names'], [name])

    def test_runtime_spec_source_cwd_and_untouched_ctest_guards(self):
        for ident, name in CHECKS.items():
            expected = [SPEC] if name == 'test_nip49_spec_source' else []
            self.assertEqual(self.policy['checks'][ident]['runtime_inputs'], expected)
            self.assertEqual(self.export['primary'][name]['runtime_inputs'], expected)
            props = r._properties(self.tests[name])
            self.assertEqual(props['LABELS'], ['selection:nip49.test_inputs'])
            self.assertEqual(props['WORKING_DIRECTORY'], '{source}/nips/nip49/../..'
                             if expected else '{build}/nips/nip49')
            for key in ('ENVIRONMENT', 'SKIP_RETURN_CODE', 'FIXTURES_REQUIRED', 'TIMEOUT'):
                self.assertNotIn(key, props)
        self.assertIn('SPEC_MD=../../docs/nips/49.md',
                      (ROOT / SPEC.removeprefix('{source}/')).read_text().splitlines())

    def test_semantic_paths_select_family_without_catchall_workaround(self):
        for relative in ('nips/nip49/src/core/nip49.c', 'nips/nip49/tests/test_nip49_vectors.c',
                         'nips/nip49/SPEC_SOURCE', 'nips/nip19/src/nip19.c'):
            self.assertTrue((ROOT / relative).is_file(), relative)
            plan = s.make_plan(self.policy, [change(relative)], head=BASE, profiles=[PROFILE])
            self.assertTrue(set(CHECKS) <= set(plan['selected_checks']), relative)
            self.assertFalse(any(self.policy['checks'][k]['cost'] == 'live_reference'
                                 for k in plan['selected_checks']))
        weak = ['execution.grotto', 'nip49.test_inputs']
        coverage = r._impact_prerequisites(self.policy, weak)
        source = s.classify(self.policy, 'nips/nip49/src/core/nip49.c')
        self.assertFalse(set(source['nodes']) <= coverage)

    def test_wrong_executable_and_missing_target_cannot_look_like_membership(self):
        name = FAMILY[0]
        bad = copy.deepcopy(self.tests[name])
        bad['command'] = ['{build}/not-the-nip49-artifact']
        with self.assertRaisesRegex(r.RealizationError, 'lacks explicit inputs/targets'):
            r._mapped_targets(bad, self.descriptors[name], self.real)
        bad_real = copy.deepcopy(self.real)
        for key in list(bad_real['targets']):
            if bad_real['targets'][key]['name'] == name:
                del bad_real['targets'][key]
        with self.assertRaises(r.RealizationError):
            r._mapped_targets(self.tests[name], self.descriptors[name], bad_real)

    def test_precise_selected_arrays_and_full_prior_record_projection(self):
        contract = self.policy['diagnostic_nonmember_subsets'][PROFILE]
        previous = before_grotto_nip49_family(contract)
        authored = sorted(n for v in self.policy['checks'].values()
                          for n in v.get('ctest_tests', {}).get(PROFILE, []))
        self.assertEqual(contract['primary_registrations'], authored)
        self.assertEqual((len(authored), len(contract['required_closure'])), (58, 61))
        self.assertEqual((len(previous['primary_registrations']), len(previous['required_closure'])), (53, 56))
        self.assertEqual(len(contract['registrations']), 33)
        self.assertEqual(len(contract['review']['source_inputs']), 193)
        self.assertTrue(set(FAMILY).isdisjoint(contract['registrations']))
        self.assertEqual(s.digest(previous), self.renewal['previous_record_hash'])
        for key in ('candidate_contract_hash', 'profile_hash', 'review', 'registration_universe',
                    'toolchain_contract_hash', 'registrations'):
            bad = copy.deepcopy(contract)
            bad[key] = 'changed-authority'
            with self.assertRaisesRegex(s.ContractError, 'non-selection authority'):
                before_grotto_nip49_family(bad)
        bad = copy.deepcopy(contract)
        bad['required_closure'].append('test_nip44_vectors')
        with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
            before_grotto_nip49_family(bad)
        with self.assertRaisesRegex(s.ContractError, 'selected closure changed'):
            before_grotto_nip49_family(previous)

    def test_source_bound_history_other_authorities_and_capabilities_preserved(self):
        self.assertEqual(self.export['source_identity'], {'kind': 'committed', 'sha': BASE})
        self.assertEqual(self.renewal['source_identity'], self.export['source_identity'])
        self.assertEqual(self.export['realization_sha256'], self.renewal['fresh_realization_sha256'])
        self.assertEqual(self.policy['profiles'][PROFILE], self.export['profile'])
        for key in ('profiles', 'diagnostic_nonmembers'):
            self.assertEqual(s.digest(self.policy[key]), self.renewal['unchanged_contract_hashes'][key])
        self.assertEqual(s.digest(self.policy['diagnostic_nonmember_subsets']['groundhog.linux']),
                         self.renewal['unchanged_contract_hashes']['groundhog_subset'])
        for path, digest in self.renewal['historical_fixture_sha256'].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), digest, path)
        for name, required in [('signer/approval-dialog', ['display']), ('signer/dbus', ['private_bus'])]:
            self.assertEqual(self.export['protected_descriptors'][name]['requires'], required)
        self.assertEqual(self.policy['checks']['grotto.signer.native'], self.renewal['previous_signer_check'])
        sources = {path.removeprefix('{source}/') for primary in self.export['primary'].values()
                   for path in primary['source_inputs'] + primary['runtime_inputs']}
        self.assertTrue(sources <= self.renewal['reviewed_source_sha256'].keys())

    def test_physical_diagnostic_is_capability_honest_not_promotion(self):
        proof = self.renewal['physical']
        self.assertEqual(proof['source_identity'], self.export['source_identity'])
        self.assertEqual(proof['required_capabilities'], [])
        self.assertFalse(any(proof['headless_environment'].values()))
        self.assertEqual(proof['build']['exit_code'], 0)
        self.assertEqual(proof['test']['exit_code'], 0)
        self.assertEqual(proof['test']['skips'], 0)
        self.assertEqual(proof['test']['executed'], FAMILY)
        self.assertIn('raw diagnostic', proof['execution'])
        self.assertFalse(any(self.renewal['limits'].values()))

    def test_new_files_are_selection_contract_runtime_inputs(self):
        for path in (EXPORT, RENEWAL, 'tests/selection/grotto_nip49_projection.py',
                     'tests/selection/test_grotto_nip49_membership.py'):
            self.assertIn('{source}/' + path, self.policy['checks']['selection.contract']['runtime_inputs'])


if __name__ == '__main__':
    unittest.main()
