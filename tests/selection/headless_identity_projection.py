"""Project the exact later selected-family delta when inspecting older receipts."""
import copy
import json

from selector_support import ROOT, s

RENEWAL = 'tests/selection/fixtures/headless_identity_subset_renewal_20261022.json'
FAMILY = [
    'groundhog-account-backends',
    'groundhog-account-remote-switch',
    'groundhog-nip46-credentials-list',
    'groundhog-nip46-session',
    'groundhog-signer-nip46',
]


def before_identity_family(contract):
    witness = json.loads((ROOT / RENEWAL).read_text())
    s.require(witness['added_primary'] == FAMILY, 'identity renewal family changed')
    previous = copy.deepcopy(contract)
    for key in ('primary_registrations', 'required_closure'):
        expected = sorted(witness['previous'][key] + FAMILY)
        s.require(contract[key] == witness['renewed'][key] == expected,
                  'identity renewal selected closure changed')
        previous[key] = witness['previous'][key]
    s.require(s.digest(previous) == witness['previous_record_hash'],
              'identity renewal changed non-selection authority')
    return previous
