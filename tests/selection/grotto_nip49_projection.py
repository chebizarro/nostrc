"""Source-bound historical projection; never admits old selected arrays to a run."""
import copy
import json

from selector_support import ROOT, s

BASE = 'ed70631704778a7fbe31d45b24161570a91abb53'
PROFILE = 'grotto.linux'
EXPORT = 'tests/selection/fixtures/grotto_nip49_exports.json'
RENEWAL = 'tests/selection/fixtures/grotto_nip49_subset_renewal_20261022.json'
FAMILY = ['test_nip49_negative', 'test_nip49_payload', 'test_nip49_roundtrip',
          'test_nip49_vectors']


def before_grotto_nip49_family(contract):
    witness = json.loads((ROOT / RENEWAL).read_text())
    s.require(witness['source_identity'] == {'kind': 'committed', 'sha': BASE},
              'Grotto NIP-49 renewal source witness changed')
    s.require(witness['added_primary'] == FAMILY, 'Grotto NIP-49 renewal family changed')
    previous = copy.deepcopy(contract)
    for key in ('primary_registrations', 'required_closure'):
        expected = sorted(witness['previous'][key] + FAMILY)
        s.require(contract[key] == witness['renewed'][key] == expected,
                  'Grotto NIP-49 renewal selected closure changed')
        previous[key] = witness['previous'][key]
    s.require(s.digest(previous) == witness['previous_record_hash'],
              'Grotto NIP-49 renewal changed non-selection authority')
    return previous


def grotto_nip49_current_workflow_checks(previous_checks, contract):
    before_grotto_nip49_family(contract)
    witness = json.loads((ROOT / RENEWAL).read_text())
    s.require(s.digest(previous_checks) == witness['previous_workflow_checks_hash'],
              'Grotto NIP-49 historical workflow checks changed')
    return sorted(previous_checks + ['grotto.' + name.removeprefix('test_') for name in FAMILY])
