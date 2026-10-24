"""Exact post-H integration inverse for historical witnesses, never admission.

H aebb6eff6 (W11 batch2, pushed and hosted-tested) is frozen together with its
witness. After H, nostrc-0cfsg.36.27 (14af5ede5, one canonical source-claim
contract) changed scripts/test_selection_services.py and
scripts/test_selection_execution.py; this layer records each exact
current->H delta once (plus the byte-pinned historical post-1640 test it now
projects) and its clerical selection.contract enrollment. nostrc-0cfsg.36.26
(3416f85ab, bounded Static scheduling with an owned host supervisor) adds exact
records for .github/workflows/static-checks.yml and
tests/selection/test_namespace_policy_contract.py (restored_sha256 = exactly
post-1640's before, the state post-1640 re-offers), the exact static-checks
workflow_jobs map (reversed by before_policy and before_jobs) and the
static_frontend_host.py enrollment. Each record maps only
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
SHA256 = '9abce5ad3d32703827270570a7c679d73fff5aee7ef1bfc54858e0655505979e'
RECORD_FIELDS = {'before_sha256', 'after_sha256', 'authors', 'origin', 'edits', 'restored_sha256'}


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'post-H integration witness changed')
    return json.loads(raw)


def before_source(path, raw):
    """Chain entry: newer layers (post-1f7, then post-419) first, then this layer's own inverse."""
    from w11_post_1f7_integration_support import before_source as before_newer_source
    return before_own_source(path, before_newer_source(path, raw))


def before_own_source(path, raw):
    """This layer's own strict inverse only (exactly the pre-split before_source body)."""
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
    # The newer post-1f7 layer's clerical enrollment is projected first.
    from w11_post_1f7_integration_support import before_policy as before_newer_policy
    data = witness()
    previous = before_newer_policy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    jobs = data['maps']['workflow_jobs']
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'post-H enrollment partially stripped')
        for ident, record in jobs.items():
            s.require(previous['workflow_jobs'].get(ident) == record['after'], 'post-H workflow job changed: ' + ident)
            previous['workflow_jobs'][ident] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'post-H input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        for ident, record in jobs.items():
            s.require(previous['workflow_jobs'].get(ident) != record['after'],
                      'post-H workflow job hashes without enrollment: ' + ident)
    return previous


def before_jobs(records):
    """Exact live job hashes -> pre-.36.26 hashes; anything else passes to older guards."""
    previous = copy.deepcopy(records)
    fields = ('source_sha256', 'workflow_sha256', 'matrix_source')
    for ident, record in witness()['maps']['workflow_jobs'].items():
        if ident in previous and all(previous[ident].get(key) == record['after'][key] for key in fields):
            previous[ident].update({key: record['before'][key] for key in fields})
    return previous
