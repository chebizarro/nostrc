"""Exact approved post-1640 inverse for historical tests, never admission.

Only the frozen A/C authoring and its clerical enrollment are reversed. Fresh
normal execution never consults this witness or pins product source bytes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/post1640_integration_20261024.json'
SHA256 = 'b5315a2809c158f2759082ffea53a05e2bd10d55cb944bfa32525af86a1e4c45'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'post-1640 witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None or hashlib.sha256(raw).hexdigest() == record['before_sha256']:
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record['after_sha256'],
              'post-1640 source changed: ' + path)
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous post-1640 source inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'],
              'post-1640 historical source changed: ' + path)
    return raw


def before_jobs(records):
    previous = copy.deepcopy(records)
    fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
    for ident, record in witness()['maps']['workflow_jobs'].items():
        after = {key: record['after'][key] for key in fields}
        if record['before'] is None:
            # Only this exact new source job disappears from historical views.
            # Unknown jobs, extra fields and changed hashes remain unprojected.
            if previous.get(ident) == after:
                del previous[ident]
        elif ident in previous and all(previous[ident].get(key) == after[key] for key in fields):
            previous[ident].update({key: record['before'][key] for key in fields})
    return previous


def before_policy(policy):
    data = witness()
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'post-1640 enrollment partially stripped')
        for field, records in data['maps'].items():
            for ident, record in records.items():
                s.require(previous[field].get(ident) == record['after'],
                          'post-1640 metadata changed: ' + ident)
                if record['before'] is None:
                    del previous[field][ident]
                else:
                    previous[field][ident] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'post-1640 input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        for field, records in data['maps'].items():
            for ident, record in records.items():
                s.require(previous[field].get(ident) == record['before'],
                          'post-1640 historical metadata changed: ' + ident)
    return previous
