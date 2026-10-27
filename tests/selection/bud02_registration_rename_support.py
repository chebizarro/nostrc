"""Exact inverse of the BUD-02 extraction registration rename, never admission.

4310ade58 renamed ctest registration test-hanami-bud02-auth to test-bud02-auth
when the codec moved to nips/bud02, and rewrote the inventory's sorted
registration lists accordingly without renewing their pinned hashes. This layer
reverses exactly that one name at the recorded list paths (witness pinned) so
the reviewed historical lists, the ordinary-subset universe hashes and the
reviewed groundhog.linux.sanitizers digest are reproduced byte-exactly. Any
other difference is left for the older exact layers to refuse. It is the newest
layer, projected first.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/bud02_registration_rename_20261027.json'
SHA256 = 'f2fe55bbf6f8a78828b19c20e923c04b4e1ac85e1fda11f1f2f92558752f37d9'


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'bud02 registration rename witness changed')
    return json.loads(raw)


def before_registrations(names):
    """Reverse the rename in one sorted unique registration list (a new list)."""
    data = witness()
    old, new = data['old'], data['new']
    s.require(names == sorted(names) and len(set(names)) == len(names), 'registration list is not sorted/unique')
    s.require((old in names) != (new in names), 'bud02 rename is not exactly one-sided')
    if new not in names:
        return list(names)
    return sorted(old if name == new else name for name in names)


def _locate(previous, path):
    section, key, field = path
    if section == 'coverage_gaps':
        matches = [gap for gap in previous[section] if gap.get('id') == key]
        s.require(len(matches) == 1, 'bud02 rename coverage gap missing/duplicated: ' + key)
        return matches[0], field
    return previous[section][key], field


def before_policy(policy):
    from inventory_option_drift_support import before_policy as before_newer_policy
    previous = before_newer_policy(policy)
    for path in witness()['paths']:
        holder, field = _locate(previous, path)
        holder[field] = before_registrations(holder[field])
    return previous
