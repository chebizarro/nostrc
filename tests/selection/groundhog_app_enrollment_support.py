"""Exact approved groundhog.linux 46-app ownership repair inverse, never admission.

Hosted run 40044808377 job 120724058820 (tested f5d85dec8) failed its pre-build
audit once the pinned Blueprint configure succeeded: 46 actual app registrations
were unclaimed (316 missing registration edges) and groundhog.default_build
lacked the six app nodes its default build compiles (322 edges). Descriptors of
the 46 and their three support fixtures equal same-source groundhog.mdk08's,
whose audit passed with zero gaps under the same nine supported.apps checks.
Only those nine projections, the six nodes and this layer's clerical enrollment
are reversed for historical views. This is the newest exact layer; it changes
no source bytes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/groundhog_app_enrollment_20261024.json'
SHA256 = '5d2abdde5c5f14372ae6528d5569463905393cd9d27a28c3fdaa44e78347097e'
PROFILE = 'groundhog.linux'
DEFAULT = 'groundhog.default_build'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'groundhog app enrollment witness changed')
    return json.loads(raw)


def before_policy(policy):
    data = witness()
    previous = copy.deepcopy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    records = data['maps']['checks']
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'groundhog app enrollment partially stripped')
        for ident, record in records.items():
            s.require(previous['checks'].get(ident) == record['after'],
                      'groundhog app enrollment check changed: ' + ident)
            previous['checks'][ident] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'groundhog app enrollment input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        # Older exact layers keep their own strict guards on these checks. This
        # layer refuses only its own projection surviving without its enrollment.
        for ident in records:
            check = previous['checks'].get(ident)
            if check is None:
                continue  # an older view without the check cannot carry this projection
            if ident == DEFAULT:
                s.require(not set(data['added_default_nodes']) & set(check['nodes']),
                          'groundhog default-build nodes without enrollment')
            else:
                s.require(PROFILE not in check['profiles'] and PROFILE not in check.get('ctest_tests', {}),
                          'groundhog app projection without enrollment: ' + ident)
    return previous
