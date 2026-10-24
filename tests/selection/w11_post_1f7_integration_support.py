"""Exact post-1f7 integration inverse for historical witnesses, never admission.

H' 1f7dc22b6 (PR26 head, pushed and hosted-tested) is frozen together with every
JSON witness it carries. After it, nostrc-0cfsg.36.26 (87345c7e0 + d70584811)
removed measured redundant fixture work from two unit-test files; only
tests/selection/test_normal_execution_contract.py is byte-pinned, so this layer
records its exact current->1f7 delta once, plus its own clerical
selection.contract enrollment. Its before bytes (1f7) and exactly the older
states the H-era batch2 layer accepts unchanged for that path (that record's
before and restored_sha256) pass on to the older strict inverses; any other
bytes refuse. No runtime, qualifier, authority, inventory metadata, path rule,
profile, capability, omission or review binding changes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_post_1f7_integration_20261024.json'
SHA256 = 'b65de61ce23d029d91673b296dd67125fae9fa877aa8b5aadb65d908cb76f4de'
RECORD_FIELDS = {'before_sha256', 'after_sha256', 'authors', 'origin', 'edits', 'restored_sha256'}


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'post-1f7 integration witness changed')
    return json.loads(raw)


def before_source(path, raw):
    # The newer post-419 layer (.36.26 c2320daf0 bytes) is projected first.
    from w11_post_419_integration_support import before_source as before_newer_source
    raw = before_newer_source(path, raw)
    record = witness()['sources'].get(path)
    if record is None:
        return raw
    s.require(set(record) == RECORD_FIELDS and isinstance(record['edits'], list) and record['edits']
              and isinstance(record['restored_sha256'], list), 'malformed post-1f7 integration record: ' + path)
    digest = hashlib.sha256(raw).hexdigest()
    if digest == record['before_sha256'] or digest in record['restored_sha256']:
        return raw
    s.require(digest == record['after_sha256'], 'post-1f7 integration source changed: ' + path)
    for edit in record['edits']:
        s.require(isinstance(edit, dict) and set(edit) == {'before', 'after'}, 'malformed post-1f7 integration edit: ' + path)
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous post-1f7 integration inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'], 'post-1f7 historical source changed: ' + path)
    return raw


def before_policy(policy):
    # The newer post-419 layer's clerical enrollment is projected first.
    from w11_post_419_integration_support import before_policy as before_newer_policy
    data = witness()
    previous = before_newer_policy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'post-1f7 enrollment partially stripped')
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'post-1f7 input missing/duplicated: ' + path)
            inputs.remove(path)
    return previous
