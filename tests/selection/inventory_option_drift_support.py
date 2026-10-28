"""Exact inverse of later inventory option/node edits for historical views, never admission.

Two post-17f67593e edits changed policy fields that the older W11, groundhog
and post-1640 layers compare exactly: nip65 nodes on groundhog.default_build
and its TestInventory.cmake runtime input (73e8bc7fc), and the vaultctl-prerequisite configure options prepended to seven
profiles (8d5c8d9b5). This layer lists those items by exact index and value and
removes only them; any other difference still reaches the older layers and
refuses there. It is the newest layer, projected first.
"""
import copy
import hashlib
import json
from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/inventory_option_drift_20261027.json'
SHA256 = 'b4f244a17f25c0f5b812bccd3942aa6f94e239fbc8549cf9a76d33ed2c7f97f4'
RECORD_FIELDS = {'section', 'ident', 'field', 'origin', 'items'}


def witness():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256, 'inventory option drift witness changed')
    return json.loads(raw)


def before_policy(policy):
    previous = copy.deepcopy(policy)
    for record in witness()['records']:
        s.require(set(record) == RECORD_FIELDS and record['items'], 'malformed inventory option drift record')
        ident = record['ident']
        entry = previous[record['section']].get(ident)
        if entry is None:
            continue  # an older view without the entry cannot carry the edit
        values = entry[record['field']]
        items = record['items']
        indexes = [item['index'] for item in items]
        s.require(indexes == sorted(set(indexes)), 'malformed inventory option drift indexes: ' + ident)
        present = [item['index'] < len(values) and values[item['index']] == item['value'] for item in items]
        if any(item['value'] in values for item in items):
            s.require(all(present), 'inventory option drift partially present: ' + ident)
            for index in reversed(indexes):
                del values[index]
    return previous
