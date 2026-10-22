"""Exact integration projection for immutable historical tests, never admission."""
import copy
import hashlib
import json

from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/closeout_interfaces.json'
WITNESS_SHA256 = 'f67b49bf22c5ac2f6c1d8311a6627c3db0244b6d79750316ccf2b03415329fe1'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == WITNESS_SHA256,
              'closeout interface witness bytes changed')
    return json.loads(raw)


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
    s.require(s.digest(previous) == record['before_hash'],
              'closeout historical contract changed: ' + profile)
    return previous


def before_closeout_policy(policy):
    """Reverse exact B/C interfaces and explicit default builds, not unknown edits."""
    data = witness()
    previous = copy.deepcopy(policy)
    if s.digest(previous) == data['before_policy_hash']:
        return previous
    for field in ('profiles', 'checks', 'workflow_jobs'):
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
