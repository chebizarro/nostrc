"""Exact approved MDK011 nine-check app enrollment inverse, never admission.

Hosted run 40023343699 job 120653340454 failed its pre-build audit: 46 actual
groundhog.mdk011 registrations were unclaimed (316 missing impact edges only
on them). Their descriptors equal groundhog.mdk08's, already claimed by the
same nine checks. Only those checks' added groundhog.mdk011 projection and the
clerical enrollment of this witness are reversed for historical views.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/mdk011_app_enrollment_20261024.json'
SHA256 = '3947151dce9f5e90a2d3b7420cf260baddd4361c92f2534533322739a9cc4fd8'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'MDK011 app enrollment witness changed')
    return json.loads(raw)


def before_policy(policy):
    from coreasan_app_enrollment_support import before_policy as before_newer_policy
    data = witness()
    previous = before_newer_policy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    records = data['maps']['checks']
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'MDK011 app enrollment partially stripped')
        for ident, record in records.items():
            s.require(previous['checks'].get(ident) == record['after'],
                      'MDK011 app enrollment check changed: ' + ident)
            previous['checks'][ident] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'MDK011 app enrollment input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        for ident, record in records.items():
            s.require(previous['checks'].get(ident) == record['before'],
                      'MDK011 app enrollment historical check changed: ' + ident)
    return previous

# Integration of this layer with the independent worker-prerequisite layer
# (nostrc-0cfsg.36.12/.15) changed only the post-1640 composition hooks. This
# exact projection restores the worker layer's reviewed bytes so its own strict
# source witness applies unchanged; any other bytes pass through to the strict
# older inverses.
COMPOSED_SOURCES = {
    'tests/selection/post1640_integration_support.py': {
        'composed_sha256': '349e67c93614efdb648890c3c00f6aebf3d2651f73e7cffcee1dcf5c79137433',
        'worker_sha256': '90e5e9fa582af39f8a1288acdef773af929f864c0046161229881d23b84e6840',
        'edits': [
            ('def before_source(path, raw):\n    from mdk011_app_enrollment_support import before_source as before_mdk011_source\n    from worker_prerequisite_support import before_source as before_newer_source\n    raw = before_newer_source(path, before_mdk011_source(path, raw))\n',
             'def before_source(path, raw):\n    from worker_prerequisite_support import before_source as before_newer_source\n    raw = before_newer_source(path, raw)\n'),
            ('def before_policy(policy):\n    # Two independent newer exact layers on disjoint policy fields: the MDK011\n    # (with core ASan) check projections and the worker prerequisite job hashes.\n    # Each keeps its own strict guards; their composition order is immaterial.\n    from mdk011_app_enrollment_support import before_policy as before_mdk011_policy\n    from worker_prerequisite_support import before_policy as before_worker_policy\n    data = witness()\n    previous = before_worker_policy(before_mdk011_policy(policy))\n',
             'def before_policy(policy):\n    from worker_prerequisite_support import before_policy as before_newer_policy\n    data = witness()\n    previous = before_newer_policy(policy)\n'),
        ],
    },
}


def before_source(path, raw):
    from coreasan_app_enrollment_support import before_source as before_newer_source
    raw = before_newer_source(path, raw)
    record = COMPOSED_SOURCES.get(path)
    if record is None or hashlib.sha256(raw).hexdigest() != record['composed_sha256']:
        return raw
    for after, before in record['edits']:
        s.require(raw.count(after.encode()) == 1, 'ambiguous MDK011 composition source inverse: ' + path)
        raw = raw.replace(after.encode(), before.encode(), 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['worker_sha256'],
              'MDK011 composition historical source changed: ' + path)
    return raw
