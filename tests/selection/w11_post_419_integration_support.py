"""Exact post-419 integration inverse for historical witnesses, never admission.

H3 4198376a6 (PR26 head, pushed; Static 816s success retained literal) is frozen
together with every JSON witness it carries. After it, nostrc-0cfsg.36.26
c2320daf0 tightened the Static host ownership-record predicate (token must be
64 lowercase hex in start's exact launch argv before process inspection) and
added forged-record subtests; only tests/selection/test_namespace_policy_contract.py
is byte-pinned (post-H record after = H3 bytes), so this layer records its exact
current->H3 delta once, plus its own clerical selection.contract enrollment.
static_frontend_host.py also changed but carries no witness byte pin. Its before
bytes (H3) and exactly the older states the post-H layer accepts unchanged for
that path (that record's before and restored_sha256) pass on; any other bytes
refuse. No runtime, qualifier, authority, inventory metadata, path rule,
profile, capability, omission or review binding changes.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/w11_post_419_integration_20261024.json'
SHA256 = 'cbb1dc440ece8fea8387748802858b09e670dc65b20c1ad5f75a4d3ac8d2c209'
RECORD_FIELDS = {'before_sha256', 'after_sha256', 'authors', 'origin', 'edits', 'restored_sha256'}


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'post-419 integration witness changed')
    return json.loads(raw)


def before_source(path, raw):
    record = witness()['sources'].get(path)
    if record is None:
        return raw
    s.require(set(record) == RECORD_FIELDS and isinstance(record['edits'], list) and record['edits']
              and isinstance(record['restored_sha256'], list), 'malformed post-419 integration record: ' + path)
    digest = hashlib.sha256(raw).hexdigest()
    if digest == record['before_sha256'] or digest in record['restored_sha256']:
        return raw
    s.require(digest == record['after_sha256'], 'post-419 integration source changed: ' + path)
    for edit in record['edits']:
        s.require(isinstance(edit, dict) and set(edit) == {'before', 'after'}, 'malformed post-419 integration edit: ' + path)
        after, before = edit['after'].encode(), edit['before'].encode()
        s.require(raw.count(after) == 1, 'ambiguous post-419 integration inverse: ' + path)
        raw = raw.replace(after, before, 1)
    s.require(hashlib.sha256(raw).hexdigest() == record['before_sha256'], 'post-419 historical source changed: ' + path)
    return raw


def before_policy(policy):
    from inventory_option_drift_support import before_policy as before_newer_policy
    data = witness()
    previous = before_newer_policy(policy)
    # The permission-repeat check postdates this immutable historical view.
    # Reverse only its exact enrollment; do not alter any predecessor check.
    ident = 'grotto.permissions.repeat'
    profiles = ['core.linux', 'core.linux.asan', 'grotto.linux',
                'grotto.linux.asan_ubsan', 'groundhog.linux',
                'groundhog.mdk011', 'groundhog.mdk08']
    expected = {
        'nodes': ['grotto.daemon', 'grotto.gui', 'nip55l.dbus_contract',
                  'nip55l.glib', 'nip55l.test_inputs', 'testing.common_inputs'],
        'profiles': profiles, 'kind': 'ctest', 'cost': 'ordinary',
        'requirement': 'blocking', 'expected_outcome': 'pass',
        'ctest_tests': {p: ['signer/permission-repeat'] for p in profiles},
        'build_targets': ['test-permission-repeat'], 'runtime_inputs': [],
        'requires': ['private_bus'],
    }
    if ident in previous['checks']:
        s.require(previous['checks'][ident] == expected,
                  'permission-repeat enrollment changed in historical view')
        del previous['checks'][ident]
    inputs = previous['checks']['selection.contract']['runtime_inputs']
    present = [path for path in data['runtime_inputs'] if path in inputs]
    if present:
        s.require(len(present) == len(data['runtime_inputs']), 'post-419 enrollment partially stripped')
        for path in data['runtime_inputs']:
            s.require(inputs.count(path) == 1, 'post-419 input missing/duplicated: ' + path)
            inputs.remove(path)
    return previous
