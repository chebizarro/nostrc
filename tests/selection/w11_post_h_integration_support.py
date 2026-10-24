"""Exact post-H integration inverse for historical witnesses, never admission.

H aebb6eff6 (W11 batch2, pushed and hosted-tested) is frozen together with its
witness. After H, nostrc-0cfsg.36.27 (14af5ede5, one canonical source-claim
contract) changed scripts/test_selection_services.py and
scripts/test_selection_execution.py; this layer records each exact
current->H delta once (plus the byte-pinned historical post-1640 test it now
projects) and its clerical selection.contract enrollment. Each record maps only
its exact after bytes. Its before bytes (H) and exactly the older states the
H-era batch2 layer itself accepts unchanged (that record's before and
restored_sha256) pass on to the older strict inverses; any other bytes refuse.
Records for further post-H sources are added here only as explicit before/after
records. No path rule, authority, profile, capability, omission or review
binding changes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_post_h_integration_20261024.json'
SHA256 = '77fc4064141241a73bdc204e543c9c943a7f437fd1ba7901ec0e45b8b6086fe1'
RECORD_FIELDS = {'before_sha256', 'after_sha256', 'authors', 'origin', 'edits', 'restored_sha256'}


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'post-H integration witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None:
        return raw
    s.require(set(record) == RECORD_FIELDS and isinstance(record['edits'], list) and record['edits']
              and isinstance(record['restored_sha256'], list), 'malformed post-H integration record: ' + path)
    digest = hashlib.sha256(raw).hexdigest()
    if digest == record['before_sha256'] or digest in record['restored_sha256']:
        return raw
    s.require(digest == record['after_sha256'], 'post-H integration source changed: ' + path)
    for edit in record['edits']:
        s.require(isinstance(edit, dict) and set(edit) == {'before', 'after'}, 'malformed post-H integration edit: ' + path)
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous post-H integration inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'], 'post-H historical source changed: ' + path)
    return raw


def before_policy(policy):
    data = witness()
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'post-H enrollment partially stripped')
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'post-H input missing/duplicated: ' + path)
            inputs.remove(path)
    return previous
