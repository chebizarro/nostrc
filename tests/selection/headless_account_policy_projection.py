"""Exact account-policy reverse projection for older receipts, not admission."""
import copy
import hashlib
import json

from selector_support import ROOT, s

BASE = 'f646db033031a32ad0f6283f881d8506eafbcc4b'
EXPORT = 'tests/selection/fixtures/headless_account_policy_exports.json'
RENEWAL = 'tests/selection/fixtures/headless_account_policy_subset_renewal_20261022.json'
FAMILY = ['groundhog-account', 'groundhog-account-relays',
          'groundhog-auth-policy', 'groundhog-identity']


def before_account_policy_family(contract):
    from nip44_vector_projection import before_nip44_vector_renewal
    contract = before_nip44_vector_renewal(contract, 'groundhog.linux')
    witness = json.loads((ROOT / RENEWAL).read_text())
    s.require(witness['source_identity'] == {'kind': 'committed', 'sha': BASE},
              'account policy renewal source witness changed')
    s.require(witness['added_primary'] == FAMILY, 'account policy renewal family changed')
    previous = copy.deepcopy(contract)
    for key in ('primary_registrations', 'required_closure'):
        s.require(contract[key] == witness['renewed'][key] ==
                  sorted(witness['previous'][key] + FAMILY),
                  'account policy renewal selected closure changed')
        previous[key] = witness['previous'][key]
    s.require(s.digest(previous) == witness['previous_record_hash'] and
              s.digest(contract) == witness['renewed_record_hash'],
              'account policy renewal changed non-selection authority')
    return previous


def verify_historical_account_policy_fixtures():
    witness = json.loads((ROOT / RENEWAL).read_text())
    for path, expected in witness['historical_fixture_hashes'].items():
        s.require(hashlib.sha256((ROOT / path).read_bytes()).hexdigest() == expected,
                  'account policy historical fixture bytes changed: ' + path)
