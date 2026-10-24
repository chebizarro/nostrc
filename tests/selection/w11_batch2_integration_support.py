"""Exact W11 batch2 integration inverse for historical witnesses, never admission.

nostrc-0cfsg.36.25 (276145d1c, original-byte native seal), .36.18 (09c58d8bc +
cf9c33c33, per-registration signer delivery and fail-closed consumer
qualification) and .36.23 (c326500c2, Gnostr test linkage) changed eight
witness-pinned sources from batch base 1d53e6ecd; this integration's projection
of the byte-pinned post-1640 historical test is the ninth. This newest layer records each
exact after->before delta once (the two .25-only records verbatim from its
original handoff; the jointly edited services bytes regenerated from the
combined tree), the .36.9 applicability anchor's rebinding to the changed nip5f
test source and registration, and its clerical selection.contract enrollment.
Each record maps only its exact after bytes; other non-before bytes refuse, and
before bytes pass unchanged to the older layers' own strict inverses. Because
post-1640 delegates to newer layers before checking its own result, its two
records also pass exactly that layer's restored bytes (``restored_sha256``). No
historical review binding, path rule or authority changes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_batch2_integration_20261024.json'
SHA256 = 'e4142aa7edb99f18ddd8be9c2b90fb81fe3a80d46a8c014ccdc210fcf3f69e28'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'W11 batch2 integration witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    digest = hashlib.sha256(raw).hexdigest()
    if record is None or digest == record['before_sha256'] or digest in record.get('restored_sha256', ()):
        return raw
    s.require(hashlib.sha256(raw).hexdigest() == record['after_sha256'],
              'W11 batch2 integration source changed: ' + path)
    for edit in record['edits']:
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous W11 batch2 integration inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'],
              'W11 batch2 historical source changed: ' + path)
    return raw


def before_policy(policy):
    data = witness()
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    gaps = previous['coverage_gaps']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    records = data['maps']['coverage_gaps']
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'W11 batch2 enrollment partially stripped')
        for ident, record in records.items():
            matches = [index for index, gap in enumerate(gaps) if gap.get('id') == ident]
            s.require(len(matches) == 1 and gaps[matches[0]] == record['after'],
                      'W11 batch2 coverage gap changed: ' + ident)
            gaps[matches[0]] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'W11 batch2 input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        # Older exact layers keep their own guards; only this layer's rebinding
        # surviving without its enrollment refuses here.
        for ident, record in records.items():
            s.require(record['after'] not in gaps, 'W11 batch2 coverage gap rebinding without enrollment: ' + ident)
    return previous
