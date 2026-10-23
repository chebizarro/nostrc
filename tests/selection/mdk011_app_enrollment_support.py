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
    data = witness()
    previous = copy.deepcopy(policy)
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
