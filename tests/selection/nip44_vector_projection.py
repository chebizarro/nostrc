"""Pinned, test-only reversal of the exact NIP-44 renewal, never run authority."""
import copy
import hashlib
import json

from selector_support import ROOT, s

RENEWAL = 'tests/selection/fixtures/nip44_vector_renewal_20261022.json'
RENEWAL_SHA256 = 'db156f9d41ade629e6dbdf082eee79534ee64a03a619122ef2575a37e01002ae'
CHECK = 'grotto.nip44_vectors'
REGISTRATION = 'test_nip44_vectors'
SOURCES = {'nips/nip44/CMakeLists.txt', 'nips/nip44/TestInventory.cmake'}


def renewal_witness():
    raw = (ROOT / RENEWAL).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == RENEWAL_SHA256,
              'NIP-44 renewal witness bytes changed')
    witness = json.loads(raw)
    s.require(set(witness['changed_source_inputs']) == SOURCES,
              'NIP-44 renewal source controls changed')
    return witness


def before_nip44_vector_renewal(contract, profile):
    witness = renewal_witness()
    s.require(profile in witness['profiles'], 'NIP-44 renewal profile changed')
    record = witness['profiles'][profile]
    s.require(s.digest({key: contract.get(key) for key in
                        ('primary_registrations', 'required_closure')}) == record['renewed_selection_hash'],
              'NIP-44 renewal selected closure changed')
    s.require(s.digest(contract) == record['renewed_record_hash'],
              'NIP-44 renewal changed non-selection authority')
    previous = copy.deepcopy(contract)
    inputs = previous['review']['source_inputs']
    s.require(len(inputs) == record['source_input_count'] == 193,
              'NIP-44 renewal source control keys changed')
    for path, values in witness['changed_source_inputs'].items():
        s.require(inputs[path] == values['after'], 'NIP-44 renewal source control changed')
        inputs[path] = values['before']
    for key in ('primary_registrations', 'required_closure'):
        for name in record['added_primary']:
            s.require(previous[key].count(name) == 1, 'NIP-44 renewal selected closure changed')
            previous[key].remove(name)
    s.require(s.digest(previous) == record['previous_record_hash'],
              'NIP-44 renewal historical record changed')
    return previous
