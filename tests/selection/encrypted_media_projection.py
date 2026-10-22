"""Strict historical media projection; never changes production admission."""
import copy
import hashlib
import json

from selector_support import ROOT, s

BASE = 'b7aede8166cc52a6ec55faafd755a1fa4a9121d1'
EXPORT = 'tests/selection/fixtures/encrypted_media_exports.json'
RENEWAL = 'tests/selection/fixtures/encrypted_media_subset_renewal_20261022.json'
FAMILY = ['groundhog-blossom', 'groundhog-media', 'groundhog-media-cache-binding',
          'groundhog-media-purge']
ADDED = [name for name in FAMILY if name != 'groundhog-media']


def before_encrypted_media_family(contract):
    from headless_account_policy_projection import RENEWAL as ACCOUNT_RENEWAL, before_account_policy_family
    s.require(hashlib.sha256((ROOT / ACCOUNT_RENEWAL).read_bytes()).hexdigest() ==
              '2fef2fb6697e1fc2776fe3d69948ba5f0215c755f154a63e81ab345aede159f1',
              'account policy renewal witness bytes changed')
    contract = before_account_policy_family(contract)
    witness = json.loads((ROOT / RENEWAL).read_text())
    s.require(witness['source_identity'] == {'kind': 'committed', 'sha': BASE},
              'encrypted media renewal source witness changed')
    s.require(witness['added_primary'] == ADDED, 'encrypted media renewal family changed')
    previous = copy.deepcopy(contract)
    for key in ('primary_registrations', 'required_closure'):
        s.require(contract[key] == witness['renewed'][key] ==
                  sorted(witness['previous'][key] + ADDED),
                  'encrypted media renewal selected closure changed')
        previous[key] = witness['previous'][key]
    s.require(s.digest(previous) == witness['previous_record_hash'] and
              s.digest(contract) == witness['renewed_record_hash'],
              'encrypted media renewal changed non-selection authority')
    return previous


def verify_historical_media_fixtures():
    witness = json.loads((ROOT / RENEWAL).read_text())
    for path, expected in witness['historical_fixture_hashes'].items():
        s.require(hashlib.sha256((ROOT / path).read_bytes()).hexdigest() == expected,
                  'encrypted media historical fixture bytes changed: ' + path)
