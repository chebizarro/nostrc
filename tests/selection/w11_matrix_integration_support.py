"""Exact integration inverse for P3's W11 measurement capture, never admission.

P3 e1891b4b4 added tests/selection/test_w11_matrix.py and changed
scripts/w11-evidence.py. This newest layer enrolls that test (plus this
witness) and reverses only the exact reviewed w11-evidence.py delta for
historical witnesses; unknown bytes refuse. Its two checked-in docs/reviews JSON
artifacts get exact (never prefix) classifications from their actual consumers.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_matrix_integration_20261024.json'
SHA256 = 'b742cd6d12638e0af8e5c83dd7c29df9fc0266027737148d28b515f595c54b7b'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'W11 matrix integration witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None or hashlib.sha256(raw).hexdigest() == record['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record['after_sha256'],
              'W11 matrix integration source changed: ' + path)
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous W11 matrix integration inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'],
              'W11 matrix historical source changed: ' + path)
    return raw


def before_policy(policy):
    data = witness()
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    rules = [rule for rule in data['added_path_rules'] if rule in previous['path_rules']]
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'W11 matrix integration enrollment partially stripped')
        s.require(len(rules) == len(data['added_path_rules']), 'W11 matrix evidence classification partially stripped')
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'W11 matrix integration input missing/duplicated: ' + path)
            inputs.remove(path)
        for rule in data['added_path_rules']:
            s.require(previous['path_rules'].count(rule) == 1, 'W11 matrix evidence classification duplicated')
            previous['path_rules'].remove(rule)
    else:
        s.require(not rules, 'W11 matrix evidence classification without enrollment')
    return previous
