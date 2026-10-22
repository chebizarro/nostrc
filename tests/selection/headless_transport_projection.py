"""Historical projection only; old selected arrays never authorize current runs."""
import copy
import json

from selector_support import ROOT, s

BASE = '53f2e822d04bd32e243a9cf969396f2f241af83e'
EXPORT = 'tests/selection/fixtures/headless_transport_exports.json'
RENEWAL = 'tests/selection/fixtures/headless_transport_subset_renewal_20261022.json'
FAMILY = [
    'groundhog-net',
    'groundhog-relay',
    'groundhog-relay-publish',
    'groundhog-relay-publish-wire',
    'groundhog-relay-wire',
    'groundhog-tls-resumption',
]


def before_transport_family(contract):
    from grotto_nip49_projection import before_nip49_descriptor_renewal
    contract = before_nip49_descriptor_renewal(contract, 'groundhog.linux')
    witness = json.loads((ROOT / RENEWAL).read_text())
    s.require(witness['source_identity'] == {'kind': 'committed', 'sha': BASE},
              'transport renewal source witness changed')
    s.require(witness['added_primary'] == FAMILY, 'transport renewal family changed')
    previous = copy.deepcopy(contract)
    for key in ('primary_registrations', 'required_closure'):
        expected = sorted(witness['previous'][key] + FAMILY)
        s.require(contract[key] == witness['renewed'][key] == expected,
                  'transport renewal selected closure changed')
        previous[key] = witness['previous'][key]
    s.require(s.digest(previous) == witness['previous_record_hash'],
              'transport renewal changed non-selection authority')
    return previous
