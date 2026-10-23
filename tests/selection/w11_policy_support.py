"""Exact older-test projections only, never production capability admission."""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_policy_enrollment_20261023.json'
SHA256 = '56ce45c79c2f277b8d522d2e91c6da0ed5e72dc851f5bcfbc0b1c54e0eb3d2d2'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'policy enrollment witness changed')
    return json.loads(raw)


def before_policy_jobs(records):
    from w11_json_glib_support import before_jobs
    from runtime_integration_support import before_runtime_jobs
    previous = before_jobs(before_runtime_jobs(records))
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
    from w11_json_glib_support import before_source
    from w11_placement_support import before_source as before_placement_source
    from runtime_integration_support import before_runtime_source
    raw = before_source(path, before_placement_source(path, before_runtime_source(path, raw)))
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
    from w11_json_glib_support import before_policy
    from w11_placement_support import before_policy as before_placement_policy
    previous = before_policy(before_placement_policy(policy))
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
