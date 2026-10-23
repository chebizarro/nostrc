"""Finite integration inverse for historical tests only, never runtime admission.

This reverses reviewed engine/workflow/metadata changes to B591. It does not pin
normal product sources; exact33/193 witnesses remain byte-for-byte unchanged.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/runtime_integration_20261023.json'
SHA256 = '53658f0ebd04cf40e7886d7389b48edd37789a50e11658a0301cec66cc3952b7'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'runtime integration witness changed')
    return json.loads(raw)


def before_runtime_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None or hashlib.sha256(raw).hexdigest() == record['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record['after_sha256'], 'runtime integration source changed: ' + path)
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous runtime integration inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'], 'runtime historical source changed: ' + path)
    return raw


def before_runtime_jobs(records):
    previous = copy.deepcopy(records)
    fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
    for ident, record in witness()['maps'].get('workflow_jobs', {}).items():
        if ident in previous and all(previous[ident].get(key) == record['after'].get(key) for key in fields):
            previous[ident].update({key: record['before'][key] for key in fields})
    return previous


def before_approved_diagnostic_policy(policy):
    previous = copy.deepcopy(policy)
    for ident, record in witness()['approved_diagnostic_jobs'].items():
        s.require(previous['workflow_jobs'][ident] in (record['before'], record['after']),
                  'approved heartbeat diagnostic enrollment changed: ' + ident)
        previous['workflow_jobs'][ident] = copy.deepcopy(record['before'])
    return previous


def before_runtime_policy(policy):
    previous = copy.deepcopy(policy)
    data = witness()
    if not any(profile.get('normal_execution') for profile in previous.get('profiles', {}).values()):
        return previous
    for field, records in data['maps'].items():
        for ident, record in records.items():
            s.require(previous[field].get(ident) == record['after'], 'runtime integration metadata changed: ' + ident)
            previous[field][ident] = copy.deepcopy(record['before'])
    for rule in data['added_path_rules']:
        s.require(previous['path_rules'].count(rule) == 1, 'runtime integration ownership changed')
        previous['path_rules'].remove(rule)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    for path in data['runtime_inputs']:
        s.require(inputs.count(path) == 1, 'runtime integration input changed: ' + path)
        inputs.remove(path)
    return previous
