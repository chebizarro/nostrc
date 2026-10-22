"""One lossless Batch A census and strict historical projection, never admission.

The source-family edges are authored in inventory.json. This module is test-only;
production neither imports this witness nor derives policy from an export.
"""
import copy
import hashlib
import json

from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/supported_profile_membership.json'
WITNESS_SHA256 = '7de6670380ae17adc0512fb390f50ff1269fb0b4856d865716c0ea20580de875'
NEW_INPUTS = [
    '{source}/tests/selection/supported_profile_support.py',
    '{source}/tests/selection/test_supported_profile_membership.py',
    '{source}/tests/selection/fixtures/supported_profile_membership.json',
]


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == WITNESS_SHA256,
              'supported profile census witness bytes changed')
    return json.loads(raw)


def before_supported_batch_contract(contract, profile):
    """Reverse only exact selected closure and the two descriptor source hashes."""
    record = witness()['contracts'][profile]
    digest = s.digest(contract)
    if digest == record['before_hash']:
        return copy.deepcopy(contract)
    s.require(digest == record['after_hash'],
              'supported profile census selected closure changed or non-selection authority changed')
    previous = copy.deepcopy(contract)
    for key, value in record['after_selection'].items():
        s.require(previous[key] == value, 'supported profile census selected closure changed')
        previous[key] = record['before_selection'][key]
    for path, values in record['source_changes'].items():
        s.require(previous['review']['source_inputs'][path] == values['after'],
                  'supported profile census descriptor source control changed')
        previous['review']['source_inputs'][path] = values['before']
    s.require(s.digest(previous) == record['before_hash'],
              'supported profile census historical record changed')
    return previous


def before_supported_batch_policy(policy):
    """Keep immutable historical selector/registration witnesses executable.

    Unknown edits refuse instead of being projected away. Current policy is
    independently checked against the whole census in the dedicated batch tests.
    """
    data = witness()
    previous = copy.deepcopy(policy)
    for ident, change in data['inventory_changes']['checks'].items():
        s.require(previous['checks'].get(ident) == change['after'],
                  'supported profile census check changed: ' + ident)
        if change['before'] is None:
            del previous['checks'][ident]
        else:
            previous['checks'][ident] = change['before']
    gap = data['inventory_changes']['coverage_gaps']
    s.require(previous['coverage_gaps'].count(gap) == 1,
              'supported profile census remainder changed')
    previous['coverage_gaps'].remove(gap)
    for ident, node in data.get('structural_changes', {}).get('nodes', {}).items():
        s.require(previous['nodes'].get(ident) == node,
                  'supported profile census input node changed: ' + ident)
        del previous['nodes'][ident]
    for rule in data.get('structural_changes', {}).get('path_rules', []):
        s.require(previous['path_rules'].count(rule) == 1,
                  'supported profile census fixture path rule changed')
        previous['path_rules'].remove(rule)
    for profile in data['contracts']:
        previous['diagnostic_nonmember_subsets'][profile] = before_supported_batch_contract(
            previous['diagnostic_nonmember_subsets'][profile], profile)
    return previous


def supported_golden_checks(name, previous_checks, policy):
    data = witness()
    before_supported_batch_policy(policy)
    record = data['golden_updates'].get(name)
    if record is None:
        return previous_checks
    s.require(previous_checks == record['before'],
              'supported profile census prior golden selection changed')
    return record['after']


def census_inputs(data, primary):
    return [data['source_paths'][i] for i in data['input_sets'][primary['inputs']]]
