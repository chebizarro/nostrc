"""Exact historical test projection; never imported by production admission."""
import copy
import hashlib
import json

from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_worker_enrollment_20261023.json'
SHA256 = '67a41aff91b85e6341df03d8625d96bea364cff8a8cd6faa5a9a72c79321cd6d'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'worker enrollment witness changed')
    return json.loads(raw)


def current_source_hash(path, historical):
    data = witness()
    if path not in data['source_before_sha256']:
        return historical
    s.require(historical == data['source_before_sha256'][path],
              'worker historical source provenance changed: ' + path)
    return data.get('integration_source_changes', {}).get(path, {}).get('after', data['source_sha256'][path])


def before_worker_source(path, raw):
    data = witness()
    if path not in data.get('source_edits', {}):
        return raw
    if hashlib.sha256(raw).hexdigest() == data['source_before_sha256'][path]:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == data['source_sha256'][path],
              'worker workflow source changed: ' + path)
    for edit in data['source_edits'][path]:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'worker source edit is not unique')
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == data['source_before_sha256'][path],
              'worker historical source changed: ' + path)
    return raw


def before_worker_jobs(records):
    previous = copy.deepcopy(records)
    for ident, record in witness()['records'].get('workflow_jobs', {}).items():
        keys = ('source_sha256', 'workflow_sha256', 'matrix_source')
        after = {key: record['after'][key] for key in keys}
        if {key: previous[ident][key] for key in keys} == after:
            previous[ident].update({key: record['before'][key] for key in keys})
    return previous


def before_worker_contract(contract, profile):
    record = witness()['contract_refresh'].get(profile)
    if record is None or s.digest(contract) != record['after_hash']:
        return copy.deepcopy(contract)
    previous = copy.deepcopy(contract)
    previous['profile_hash'] = record['profile_hash']['before']
    for path, values in record['source_changes'].items():
        s.require(previous['review']['source_inputs'][path] == values['after'],
                  'worker current source binding changed: ' + path)
        previous['review']['source_inputs'][path] = values['before']
    s.require(s.digest(previous) == record['before_hash'], 'worker historical contract changed: ' + profile)
    return previous


def before_worker_policy(policy):
    previous = copy.deepcopy(policy)
    data = witness()
    if not any(previous[field].get(ident) == record['after']
               for field, records in data['records'].items() for ident, record in records.items()):
        return previous
    for field, records in data['records'].items():
        for ident, record in records.items():
            value = previous[field].get(ident)
            s.require(value in (record['before'], record['after']), 'worker enrollment changed: ' + ident)
            if record['before'] is None:
                previous[field].pop(ident, None)
            else:
                previous[field][ident] = copy.deepcopy(record['before'])
    for record in data['rules']:
        if record['after'] not in previous['path_rules']:
            s.require(record['before'] is None or previous['path_rules'].count(record['before']) == 1,
                      'worker historical source ownership changed')
            continue
        s.require(previous['path_rules'].count(record['after']) == 1, 'worker source ownership duplicated')
        index = previous['path_rules'].index(record['after'])
        if record['before'] is None:
            previous['path_rules'].pop(index)
        else:
            previous['path_rules'][index] = copy.deepcopy(record['before'])
    for profile, record in data['contract_refresh'].items():
        current = previous['diagnostic_nonmember_subsets'][profile]
        s.require(s.digest(current) in (record['before_hash'], record['after_hash']),
                  'worker current contract changed: ' + profile)
        previous['diagnostic_nonmember_subsets'][profile] = before_worker_contract(current, profile)
    return previous
