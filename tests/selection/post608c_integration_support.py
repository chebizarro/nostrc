"""Exact post-608c historical-test inverse; never production admission.

Approved A transport, C legacy applicability, and their finite enrollment are
reversed only for old witnesses. No product bytes or omission authority change.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/post608c_integration_20261024.json'
SHA256 = '86d1835782c978f37551b1ca33b28e4789aa9afc632599a9b607759312851b60'
HOSTED_WITNESS = 'tests/selection/fixtures/hosted_cause_integration_20261024.json'
HOSTED_SHA256 = 'd13f2572fe3dc9e8fdcc9d797bd8728fd98b8ac819b062838fa9a6108db1761b'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'post-608c witness changed')
    return json.loads(raw)


def hosted_witness():
    raw = (ROOT / HOSTED_WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == HOSTED_SHA256, 'hosted-cause witness changed')
    return json.loads(raw)


def before_hosted_source(path, raw):
    return _source(hosted_witness(), path, raw)


def before_source(path, raw):
    data = witness()
    record = data['sources'].get(path)
    # An already-restored608c source must not be offered to the newer9062 layer.
    if record is not None and hashlib.sha256(raw).hexdigest() == record['before_sha256']:
        return raw
    return _source(data, path, before_hosted_source(path, raw))


def _source(data, path, raw):
    record = data['sources'].get(path)
    if record is None or hashlib.sha256(raw).hexdigest() == record['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record['after_sha256'],
              'post-608c source changed: ' + path)
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous post-608c source inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'],
              'post-608c historical source changed: ' + path)
    return raw


def before_hosted_jobs(records):
    return _jobs(hosted_witness(), records)


def before_jobs(records):
    return _jobs(witness(), before_hosted_jobs(records))


def _jobs(data, records):
    previous = copy.deepcopy(records)
    fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
    for ident, record in data['maps']['workflow_jobs'].items():
        if ident in previous and all(previous[ident].get(key) == record['after'][key] for key in fields):
            previous[ident].update({key: record['before'][key] for key in fields})
    return previous


def before_hosted_policy(policy):
    return _policy(hosted_witness(), policy)


def before_policy(policy):
    return _policy(witness(), before_hosted_policy(policy))


def _policy(data, policy):
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    if not present:
        for field, records in data['maps'].items():
            for ident, record in records.items():
                s.require(previous[field].get(ident) == record['before'],
                          'post-608c historical metadata changed: ' + ident)
        for field, key in (('path_rules', 'added_path_rules'), ('coverage_gaps', 'added_coverage_gaps')):
            s.require(not any(item in previous[field] for item in data[key]),
                      'post-608c enrollment partially stripped: ' + field)
        return previous
    s.require(len(present) == len(data['runtime_inputs']), 'post-608c enrollment partially stripped')
    for field, records in data['maps'].items():
        for ident, record in records.items():
            s.require(previous[field].get(ident) == record['after'],
                      'post-608c metadata changed: ' + ident)
            previous[field][ident] = copy.deepcopy(record['before'])
    for field, key in (('path_rules', 'added_path_rules'), ('coverage_gaps', 'added_coverage_gaps')):
        for item in data[key]:
            s.require(previous[field].count(item) == 1, 'post-608c ownership/applicability changed: ' + field)
            previous[field].remove(item)
    for path in data['runtime_inputs']:
        s.require(inputs.count(path) == 1, 'post-608c input missing/duplicated: ' + path)
        inputs.remove(path)
    return previous
