"""Exact older-test projections only, never production capability admission."""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_policy_enrollment_20261023.json'
SHA256 = '03083907ee125de83dfaaba923fdc679fdcdc10b0d194b2162b1c5f24be1161e'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'policy enrollment witness changed')
    return json.loads(raw)


def before_policy_jobs(records):
    previous = copy.deepcopy(records)
    for ident, change in witness()['records']['workflow_jobs'].items():
        if ident not in previous:
            continue
        fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
        if all(previous[ident].get(k) == change['after'][k] for k in fields):
            if change['before'] is None:
                del previous[ident]
            else:
                previous[ident].update({k: change['before'][k] for k in fields})
    return previous


def before_policy_source(path, raw):
    change = witness()['source_edits'].get(path)
    if change is None or hashlib.sha256(raw).hexdigest() == change['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == change['after_sha256'], 'policy workflow source changed')
    for edit in change['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'policy workflow edit is not unique')
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == change['before_sha256'], 'policy historical workflow changed')
    return raw


def before_policy_enrollment(policy):
    previous = copy.deepcopy(policy)
    data = witness()
    if previous['profiles'].get('static.policy') != data['records']['profiles']['static.policy']['after']:
        return previous
    for field, changes in data['records'].items():
        for ident, change in changes.items():
            s.require(previous[field].get(ident) == change['after'], 'policy enrollment changed: ' + ident)
            if change['before'] is None:
                del previous[field][ident]
            else:
                previous[field][ident] = copy.deepcopy(change['before'])
    check = previous['checks']['selection.contract']
    for field, change in data['check_fields'].items():
        s.require(check.get(field) == change['after'], 'policy command field changed: ' + field)
        if change['before'] is None:
            check.pop(field, None)
        else:
            check[field] = copy.deepcopy(change['before'])
    for path in data['input_additions']:
        s.require(check['runtime_inputs'].count(path) == 1, 'policy source input missing/duplicated: ' + path)
        check['runtime_inputs'].remove(path)
    s.require(previous['path_rules'].count(data['path_rule']) == 1, 'policy source ownership changed')
    previous['path_rules'].remove(data['path_rule'])
    return previous
