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
records also pass exactly that layer's restored bytes (``restored_sha256``). The
.36.18 publication gate adds the dependency-worker.yml source record and the exact
execute workflow_jobs hash map (reversed first by before_jobs). No historical
review binding, path rule or authority changes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_batch2_integration_20261024.json'
SHA256 = '3c618784578606eef1c688b940ed5e691f7e028a265d9cd8e1a68353d17c029d'


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
    # The newer post-H layer (.36.27 and later) is projected first.
    from w11_post_h_integration_support import before_policy as before_newer_policy
    data = witness()
    previous = before_newer_policy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    gaps = previous['coverage_gaps']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    records = data['maps']['coverage_gaps']
    jobs = data['maps']['workflow_jobs']
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'W11 batch2 enrollment partially stripped')
        for ident, record in jobs.items():
            s.require(previous['workflow_jobs'].get(ident) == record['after'], 'W11 batch2 workflow job changed: ' + ident)
            previous['workflow_jobs'][ident] = copy.deepcopy(record['before'])
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
        for ident, record in jobs.items():
            s.require(previous['workflow_jobs'].get(ident) != record['after'],
                      'W11 batch2 workflow job hashes without enrollment: ' + ident)
    return previous


def before_jobs(records):
    """Exact live execute-job hashes -> batch base; anything else passes to older guards."""
    # The newer post-H layer (.36.26 static-checks job) is projected first.
    from w11_post_h_integration_support import before_jobs as before_post_h_jobs
    previous = before_post_h_jobs(records)
    fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
    for ident, record in witness()['maps']['workflow_jobs'].items():
        if ident in previous and all(previous[ident].get(key) == record['after'][key] for key in fields):
            previous[ident].update({key: record['before'][key] for key in fields})
    return previous
