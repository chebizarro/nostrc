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
