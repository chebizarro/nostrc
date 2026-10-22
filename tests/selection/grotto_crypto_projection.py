"""Strict test-only reverse projection of the six-check ordinary Grotto wave."""
import copy
import hashlib
import json

from selector_support import ROOT, s

BASE = 'b7aede8166cc52a6ec55faafd755a1fa4a9121d1'
PROFILE = 'grotto.linux'
EXPORT = 'tests/selection/fixtures/grotto_crypto_exports.json'
RENEWAL = 'tests/selection/fixtures/grotto_crypto_subset_renewal_20261022.json'
RENEWAL_SHA256 = '315c7a0cc8f16a646d474e095b6bd5edcf47b6825b5698b2a69e905738e34743'
FAMILY = ['test_nip04_error_uniformity', 'test_nip04_secure_roundtrip',
          'test_nip04_strict', 'test_nip44_convkey', 'test_nip44_roundtrip',
          'test_roundtrip']
CHECKS = {'grotto.' + ('nip04_roundtrip' if name == 'test_roundtrip' else
                       name.removeprefix('test_')): name for name in FAMILY}


def renewal_witness():
    raw = (ROOT / RENEWAL).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == RENEWAL_SHA256,
              'Grotto crypto renewal witness bytes changed')
    witness = json.loads(raw)
    s.require(witness['source_identity'] == {'kind': 'committed', 'sha': BASE}
              and witness['profile'] == PROFILE and witness['added_primary'] == FAMILY,
              'Grotto crypto renewal source/family changed')
    return witness


def before_grotto_crypto_family(contract):
    """Project only reviewed selection fields; never renew production authority."""
    from nip44_vector_projection import before_nip44_vector_renewal
    contract = before_nip44_vector_renewal(contract, PROFILE)
    witness = renewal_witness()
    for key in ('primary_registrations', 'required_closure'):
        s.require(contract[key] == witness['renewed'][key] ==
                  sorted(witness['previous'][key] + FAMILY),
                  'Grotto crypto renewal selected closure changed')
    s.require(s.digest(contract) == witness['renewed_record_hash'],
              'Grotto crypto renewal changed non-selection authority')
    previous = copy.deepcopy(contract)
    for key in ('primary_registrations', 'required_closure'):
        previous[key] = witness['previous'][key]
    s.require(s.digest(previous) == witness['previous_record_hash'],
              'Grotto crypto renewal historical record changed')
    return previous


def grotto_crypto_current_workflow_checks(previous_checks, contract):
    before_grotto_crypto_family(contract)
    witness = renewal_witness()
    s.require(s.digest(previous_checks) == witness['previous_workflow_checks_hash'],
              'Grotto crypto historical workflow checks changed')
    from nip44_vector_projection import CHECK
    return sorted(previous_checks + list(CHECKS) + [CHECK])
