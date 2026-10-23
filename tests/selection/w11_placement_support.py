"""Finite private-resource placement enrollment inverse for historical tests, not admission."""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_placement_enrollment_20261023.json'
SHA256 = '83eae9ab733e3d4e66358c3fb0829f700158a4ec75d59d7e4b151752cbb44f1e'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'placement enrollment witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None or hashlib.sha256(raw).hexdigest() == record['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record['after_sha256'], 'placement source changed: ' + path)
    for op in record['ops']:
        after, before = op['after'].encode(), op['before'].encode()
        s.require(raw.count(after) == 1, 'placement source edit is not unique: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'], 'placement historical source changed: ' + path)
    return raw


def before_policy(policy):
    previous = copy.deepcopy(policy)
    data = witness()
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [value for value in data['input_additions'] if value in inputs]
    if not present:
        return previous
    s.require(len(present) == len(data['input_additions']), 'placement enrollment partially stripped')
    for value in present:
        s.require(inputs.count(value) == 1, 'placement physical source input duplicated: ' + value)
        inputs.remove(value)
    return previous
