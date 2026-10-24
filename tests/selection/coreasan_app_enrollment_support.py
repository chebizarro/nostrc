"""Exact approved core.linux.asan 45-app ownership repair inverse, never admission.

Hosted run 40023343699 job 120653339923 failed its pre-build audit: 48 actual
core.linux.asan registrations were unclaimed. The 45 app registrations (all 291
missing impact edges) have descriptors equal to core.linux's, claimed there by
the same nine checks; they become required ASan execution. The three legacy
ASan exclusions stay unclaimed and blocking (root_asan_execution_contract).
Only this projection and its clerical enrollment are reversed.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/coreasan_app_enrollment_20261024.json'
SHA256 = '5b6462ef797631830c011c92409984294ba016f3181f086033c1987698294077'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'core ASan app enrollment witness changed')
    return json.loads(raw)


def before_source(path, raw):
    # This layer changes no source; only the exact integration layers do. The
    # newest W11 batch2 layer hands its before bytes to the W11 matrix leaf.
    from w11_batch2_integration_support import before_source as before_batch2_source
    from w11_matrix_integration_support import before_source as before_newer_source
    return before_newer_source(path, before_batch2_source(path, raw))


def before_policy(policy):
    from w11_matrix_integration_support import before_policy as before_newer_policy
    data = witness()
    previous = before_newer_policy(policy)
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    records = data['maps']['checks']
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'core ASan app enrollment partially stripped')
        for ident, record in records.items():
            s.require(previous['checks'].get(ident) == record['after'],
                      'core ASan app enrollment check changed: ' + ident)
            previous['checks'][ident] = copy.deepcopy(record['before'])
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'core ASan app enrollment input missing/duplicated: ' + path)
            inputs.remove(path)
    else:
        # Older exact layers may already have reversed these same checks further;
        # they keep their own strict guards. This layer refuses only its own
        # projection surviving without its enrollment.
        for ident in records:
            check = previous['checks'].get(ident)
            s.require(check is not None and 'core.linux.asan' not in check['profiles']
                      and 'core.linux.asan' not in check.get('ctest_tests', {}),
                      'core ASan app projection without enrollment: ' + ident)
    return previous
