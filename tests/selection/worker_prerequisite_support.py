"""Exact worker-prerequisite inverse for historical tests, never admission.

Reverses only the reviewed libmicrohttpd-dev worker provisioning repair and its
clerical enrollment back to master eaa918a0a. Bytes other than the exact
reviewed result pass through unchanged so that the older, strict inverses
still refuse them; no product source byte is pinned.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/worker_prerequisite_20261024.json'
SHA256 = 'be4ffc93691b92fde541b3c54b847659648698a193251d11492b085f74d6364b'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'worker prerequisite witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None or hashlib.sha256(raw).hexdigest() != record['after_sha256']:
        return raw
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous worker prerequisite inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'],
              'worker prerequisite historical source changed: ' + path)
    return raw


def before_jobs(records):
    # The newer W11 batch2 layer reverses the exact publication-gate job hashes first.
    from w11_batch2_integration_support import before_jobs as before_batch2_jobs
    previous = before_batch2_jobs(records)
    fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
    for ident, record in witness()['maps']['workflow_jobs'].items():
        if ident in previous and all(previous[ident].get(key) == record['after'][key] for key in fields):
            previous[ident].update({key: record['before'][key] for key in fields})
    return previous


def before_policy(policy):
    data = witness()
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'worker prerequisite enrollment partially stripped')
        for field, records in data['maps'].items():
            for ident, record in records.items():
                s.require(previous[field].get(ident) == record['after'],
                          'worker prerequisite metadata changed: ' + ident)
                previous[field][ident] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'worker prerequisite input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        for field, records in data['maps'].items():
            for ident, record in records.items():
                s.require(previous[field].get(ident) != record['after'],
                          'worker prerequisite metadata without enrollment: ' + ident)
    return previous
