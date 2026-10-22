"""Source-bound historical projection; never admits old selected arrays to a run."""
import copy
import hashlib
import json

from selector_support import ROOT, s

BASE = 'ed70631704778a7fbe31d45b24161570a91abb53'
PROFILE = 'grotto.linux'
EXPORT = 'tests/selection/fixtures/grotto_nip49_exports.json'
RENEWAL = 'tests/selection/fixtures/grotto_nip49_subset_renewal_20261022.json'
FAMILY = ['test_nip49_negative', 'test_nip49_payload', 'test_nip49_roundtrip',
          'test_nip49_vectors']


DESCRIPTOR_RENEWAL = 'tests/selection/fixtures/nip49_descriptor_renewal_20261022.json'
DESCRIPTOR_RENEWAL_SHA256 = '78e6c2fed93894ad0f71e37207af28c642f425d57ddba7b21df47b5fb42bfcd7'


def before_nip49_descriptor_renewal(contract, profile):
    if profile == 'groundhog.linux':
        from encrypted_media_projection import RENEWAL as MEDIA_RENEWAL, before_encrypted_media_family
        s.require(hashlib.sha256((ROOT / MEDIA_RENEWAL).read_bytes()).hexdigest() ==
                  'fa30aa40a5598dfafdb070215481bbf617e395e33ad431d6c2ce8c9e8aed4228',
                  'encrypted media renewal witness bytes changed')
        contract = before_encrypted_media_family(contract)
    elif profile == PROFILE:
        from grotto_crypto_projection import before_grotto_crypto_family
        contract = before_grotto_crypto_family(contract)
    raw = (ROOT / DESCRIPTOR_RENEWAL).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == DESCRIPTOR_RENEWAL_SHA256,
              'NIP-49 descriptor renewal witness bytes changed')
    witness = json.loads(raw)
    s.require(profile in witness['profiles'], 'NIP-49 descriptor renewal profile changed')
    historical = json.loads((ROOT / (RENEWAL if profile == PROFILE else
        'tests/selection/fixtures/headless_transport_subset_renewal_20261022.json')).read_text())
    for key in ('primary_registrations', 'required_closure'):
        s.require(contract[key] == sorted(historical['renewed'][key] + witness['added_primary'][profile]),
                  'NIP-49 descriptor renewal selected closure changed')
    s.require(s.digest(contract) == witness['profiles'][profile]['renewed_record_hash'],
              'NIP-49 descriptor renewal changed non-selection authority')
    previous = copy.deepcopy(contract)
    source = witness['source_path']
    s.require(previous['review']['source_inputs'][source] == witness['after'],
              'NIP-49 descriptor renewal source control changed')
    previous['review']['source_inputs'][source] = witness['before']
    for key in ('primary_registrations', 'required_closure'):
        previous[key] = historical['renewed'][key]
    s.require(s.digest(previous) == witness['profiles'][profile]['previous_record_hash'],
              'NIP-49 descriptor renewal historical record changed')
    return previous


def before_grotto_nip49_family(contract):
    contract = before_nip49_descriptor_renewal(contract, PROFILE)
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
    from grotto_crypto_projection import grotto_crypto_current_workflow_checks
    previous_wave = sorted(previous_checks + ['grotto.' + name.removeprefix('test_') for name in
                                             FAMILY + ['test_nip49_spec_source']])
    checks = s.load_inventory(ROOT / 'tests/selection/inventory.json')['checks']
    ordinary = [ident for ident in previous_wave if PROFILE in checks[ident]['profiles']]
    retained = [ident for ident in previous_wave if ident not in ordinary]
    return sorted(retained + grotto_crypto_current_workflow_checks(ordinary, contract))
