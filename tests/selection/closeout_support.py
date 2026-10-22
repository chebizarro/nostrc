"""Exact integration projection for immutable historical tests, never admission."""
import copy
import hashlib
import json

from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/closeout_interfaces.json'
WITNESS_SHA256 = '876b33360c0d19313bbe0683a7d4a436ba7175e07ee6e6e8e45ee558184fa254'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == WITNESS_SHA256,
              'closeout interface witness bytes changed')
    return json.loads(raw)


CORE_SOURCE_WITNESS = 'tests/selection/fixtures/core_source_producers_20261023.json'
CORE_SOURCE_WITNESS_SHA256 = 'a2f936b3c00f90ae4c8a7b4e94bf468a17aa6c997d1d98e7ca7bda955ba61d8a'


def core_source_witness():
    raw = (ROOT / CORE_SOURCE_WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == CORE_SOURCE_WITNESS_SHA256,
              'core source producer witness bytes changed')
    return json.loads(raw)


def before_core_producer_source(path, content):
    """Reverse only exact root producer edits for immutable historical tests."""
    record = core_source_witness()['source_edits'].get(path)
    if record is None:
        return content
    digest = hashlib.sha256(content).hexdigest()
    if digest == record['before_sha256']:
        return content
    s.require(digest == record['after_sha256'], 'core producer source changed: ' + path)
    previous = content
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(previous.count(after) == 1, 'core producer edit is not unique: ' + path)
        previous = previous.replace(after, before, 1)
    s.require(hashlib.sha256(previous).hexdigest() == record['before_sha256'],
              'historical core producer source changed: ' + path)
    return previous


def before_closeout_contract(contract, profile):
    record = witness()['contracts'].get(profile)
    if record is None:
        return copy.deepcopy(contract)
    current = s.digest(contract)
    if current in (record['before_hash'], record['historical_before_hash']):
        return copy.deepcopy(contract)
    s.require(current == record['after_hash'],
              'closeout selected closure changed or non-selection authority changed: ' + profile)
    previous = copy.deepcopy(contract)
    s.require(previous['profile_hash'] == record['after_profile_hash'],
              'closeout profile binding changed: ' + profile)
    previous['profile_hash'] = record['before_profile_hash']
    for path, values in record.get('source_changes', {}).items():
        s.require(previous['review']['source_inputs'][path] == values['after'],
                  'closeout source control changed: ' + path)
        previous['review']['source_inputs'][path] = values['before']
    if 'after_candidate_contract_hash' in record:
        s.require(previous['candidate_contract_hash'] == record['after_candidate_contract_hash'],
                  'closeout derived target metadata changed: ' + profile)
        previous['candidate_contract_hash'] = record['before_candidate_contract_hash']
    s.require(s.digest(previous) == record['before_hash'],
              'closeout historical contract changed: ' + profile)
    return previous


RUNTIME_SOURCE_RULES = [
    {'effect': 'impact', 'exact': path, 'nodes': [owner, 'selection.contract'],
     'reason': 'Standalone runtime diagnostic source closure; preserve product ownership without profile admission.'}
    for owner, path in [('grotto.gui', 'apps/grotto/data/org.nostr.Grotto.gschema.xml')] +
    [('mdk011.peer_inputs', 'tests/interop/mdk/driver-0.11/' + name)
     for name in ('Dockerfile', 'Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', 'src/main.rs')]
]
NATIVE_RUNTIME_INPUTS = [
    '{source}/scripts/adapter_namespace_custody.py',
    '{source}/tests/selection/test_namespace_custody.py',
    '{source}/scripts/test_selection_services.py',
    '{source}/tests/selection/test_native_services.py',
    '{source}/tests/selection/test_namespace_wrapper_adapter.py',
    '{source}/tests/selection/test_runtime_diagnostic_inputs.py',
    '{source}/scripts/mdk011-cache-build.py',
    '{source}/tests/runtime/mdk011-fetch.Dockerfile',
    '{source}/tests/runtime/test_mdk011_cache_contract.py',
    '{source}/tests/runtime/test_mdk011_export_custody.py',
    '{source}/tests/runtime/test_provisioned_nip5f.py',
    '{source}/tests/runtime/nip5f_approver.c',
    '{source}/gnome/groundhog/tests/app/test_secret_service.py',
    '{source}/apps/grotto/data/org.nostr.Grotto.gschema.xml',
    '{source}/tests/interop/mdk/driver-0.11/Dockerfile',
    '{source}/tests/interop/mdk/driver-0.11/Cargo.toml',
    '{source}/tests/interop/mdk/driver-0.11/Cargo.lock',
    '{source}/tests/interop/mdk/driver-0.11/rust-toolchain.toml',
    '{source}/tests/interop/mdk/driver-0.11/src/main.rs',
]


def before_native_custody_policy(policy):
    """Reverse only the exact additive input record, never renew old authority."""
    previous = copy.deepcopy(policy)
    check = previous['checks']['selection.contract']
    if set(NATIVE_RUNTIME_INPUTS) & set(check.get('runtime_inputs', [])):
        before = witness()['checks']['selection.contract']['after']
        after = copy.deepcopy(before)
        after['runtime_inputs'] += NATIVE_RUNTIME_INPUTS
        s.require(check == after, 'native custody command input enrollment changed')
        for rule in RUNTIME_SOURCE_RULES:
            s.require(previous['path_rules'].count(rule) == 1,
                      'native custody runtime source ownership changed')
            previous['path_rules'].remove(rule)
        previous['checks']['selection.contract'] = copy.deepcopy(before)
    return previous


def before_closeout_policy(policy):
    """Reverse exact B/C interfaces and explicit default builds, not unknown edits."""
    data = witness()
    previous = before_native_custody_policy(policy)
    if s.digest(previous) == data['before_policy_hash']:
        return previous
    for field in ('profiles', 'checks', 'workflow_jobs', 'nodes'):
        for ident, record in data[field].items():
            s.require(previous[field].get(ident) == record['after'],
                      'closeout ' + field + ' changed: ' + ident)
            if record['before'] is None:
                del previous[field][ident]
            else:
                previous[field][ident] = copy.deepcopy(record['before'])
    for rule in data['path_rules']:
        s.require(previous['path_rules'].count(rule) == 1,
                  'closeout evidence path rule changed')
        previous['path_rules'].remove(rule)
    for record in data['coverage_gaps']:
        s.require(previous['coverage_gaps'].count(record['after']) == 1,
                  'closeout capability remainder changed')
        index = previous['coverage_gaps'].index(record['after'])
        previous['coverage_gaps'][index] = copy.deepcopy(record['before'])
    for profile in data['contracts']:
        previous['diagnostic_nonmember_subsets'][profile] = before_closeout_contract(
            previous['diagnostic_nonmember_subsets'][profile], profile)
    return previous


def closeout_golden_checks(name, previous_checks, policy):
    data = witness()
    before_closeout_policy(policy)
    record = data['golden_updates'].get(name)
    if record is None:
        return previous_checks
    s.require(previous_checks == record['before'],
              'closeout prior golden selection changed: ' + name)
    return record['after']
