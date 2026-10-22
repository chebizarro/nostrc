"""Only the two reviewed selection deltas may precede immutable older history."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from selector_support import ROOT, s
from encrypted_media_projection import (RENEWAL as MEDIA_RENEWAL,
                                        before_encrypted_media_family)
from grotto_crypto_projection import (RENEWAL as CRYPTO_RENEWAL,
                                      before_grotto_crypto_family)
from grotto_nip49_projection import (DESCRIPTOR_RENEWAL,
                                    before_nip49_descriptor_renewal)


class MediaCryptoIntegration(unittest.TestCase):
    def setUp(self):
        self.policy = s.load_inventory(ROOT / 'tests/selection/inventory.json')
        self.contracts = self.policy['diagnostic_nonmember_subsets']

    def test_disjoint_wave_reversals_recover_exact_prior_records(self):
        for profile, path, reverse in (
                ('groundhog.linux', MEDIA_RENEWAL, before_encrypted_media_family),
                ('grotto.linux', CRYPTO_RENEWAL, before_grotto_crypto_family)):
            with self.subTest(profile=profile):
                witness = json.loads((ROOT / path).read_text())
                current = self.contracts[profile]
                snapshot = copy.deepcopy(current)
                previous = reverse(current)
                self.assertEqual(current, snapshot)
                self.assertEqual(s.digest(previous), witness['previous_record_hash'])
                self.assertEqual(current.keys(), previous.keys())
                for key in current.keys() - {'primary_registrations', 'required_closure'}:
                    self.assertEqual(current[key], previous[key], key)
                self.assertEqual(len(current['registrations']), 33)
                self.assertEqual(len(current['review']['source_inputs']), 193)
        crypto = json.loads((ROOT / CRYPTO_RENEWAL).read_text())
        self.assertEqual(s.digest(before_encrypted_media_family(self.contracts['groundhog.linux'])),
                         crypto['unchanged_groundhog_subset_hash'])

    def test_both_waves_compose_into_exact_old_descriptor_records(self):
        witness = json.loads((ROOT / DESCRIPTOR_RENEWAL).read_text())
        for profile in self.contracts:
            with self.subTest(profile=profile):
                previous = before_nip49_descriptor_renewal(self.contracts[profile], profile)
                self.assertEqual(s.digest(previous),
                                 witness['profiles'][profile]['previous_record_hash'])

    def test_composition_refuses_unknown_selection_or_authority_changes(self):
        for profile, current in self.contracts.items():
            for mutation in ('extra_primary', 'missing_primary', 'extra_fixture',
                             'duplicate_fixture', 'source_hash', 'omission',
                             'profile_hash', 'extra_field'):
                with self.subTest(profile=profile, mutation=mutation):
                    bad = copy.deepcopy(current)
                    if mutation == 'extra_primary':
                        bad['primary_registrations'].append('unreviewed')
                    elif mutation == 'missing_primary':
                        bad['primary_registrations'].pop()
                    elif mutation == 'extra_fixture':
                        bad['required_closure'].append('unreviewed')
                    elif mutation == 'duplicate_fixture':
                        bad['required_closure'].append(bad['required_closure'][0])
                    elif mutation == 'source_hash':
                        path = next(iter(bad['review']['source_inputs']))
                        bad['review']['source_inputs'][path] = '0' * 64
                    elif mutation == 'omission':
                        bad['registrations'].pop()
                    elif mutation == 'profile_hash':
                        bad['profile_hash'] = '0' * 64
                    else:
                        bad['unreviewed'] = True
                    with self.assertRaises(s.ContractError):
                        before_nip49_descriptor_renewal(bad, profile)

    def test_workflow_composition_preserves_other_profiles_and_refuses_repartition(self):
        from grotto_crypto_projection import CHECKS as CRYPTO_CHECKS
        from grotto_nip49_projection import FAMILY, grotto_nip49_current_workflow_checks
        cases = json.loads((ROOT / 'tests/selection/golden/selection.json').read_text())
        old = next(case['checks'] for case in cases if case['name'] == 'grotto_workflow_only')
        current = self.contracts['grotto.linux']
        expected = sorted(old + ['grotto.' + name.removeprefix('test_') for name in
                                 FAMILY + ['test_nip49_spec_source']] + list(CRYPTO_CHECKS))
        self.assertEqual(grotto_nip49_current_workflow_checks(old, current), expected)
        bad_policy = copy.deepcopy(self.policy)
        bad_policy['checks']['grotto.signer.native']['profiles'].remove('grotto.linux')
        with mock.patch.object(s, 'load_inventory', return_value=bad_policy):
            with self.assertRaisesRegex(s.ContractError, 'historical workflow checks changed'):
                grotto_nip49_current_workflow_checks(old, current)

    def test_account_policy_wave_precedes_media_and_retains_pinned_witness(self):
        from headless_account_policy_projection import RENEWAL, before_account_policy_family
        current = self.contracts['groundhog.linux']
        previous = before_account_policy_family(current)
        self.assertEqual((len(current['primary_registrations']), len(current['required_closure'])),
                         (34, 36))
        self.assertEqual((len(previous['primary_registrations']), len(previous['required_closure'])),
                         (30, 32))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / RENEWAL
            path.parent.mkdir(parents=True)
            path.write_bytes((ROOT / RENEWAL).read_bytes() + b'\n')
            with mock.patch('encrypted_media_projection.ROOT', root):
                with self.assertRaisesRegex(s.ContractError, 'account policy renewal witness bytes changed'):
                    before_encrypted_media_family(current)

    def test_media_witness_bytes_remain_pinned_by_shared_composition(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / MEDIA_RENEWAL
            path.parent.mkdir(parents=True)
            path.write_bytes((ROOT / MEDIA_RENEWAL).read_bytes() + b'\n')
            with mock.patch('grotto_nip49_projection.ROOT', root):
                with self.assertRaisesRegex(s.ContractError, 'media renewal witness bytes changed'):
                    before_nip49_descriptor_renewal(self.contracts['groundhog.linux'],
                                                   'groundhog.linux')


if __name__ == '__main__':
    unittest.main()
