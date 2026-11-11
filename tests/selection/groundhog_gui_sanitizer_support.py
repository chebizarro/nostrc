"""Pinned pre-GUI contract, never current diagnostic omission authority.

Mirror the SHA-pinned witness loader in test_sanitizer_partition_drift.py.
nostrc-4bi6r intentionally withdrew this exemption in e55e61b7a when GUI tests
entered sanitizer coverage; retain its historical assertions without renewal.
"""
import hashlib
import json

from selector_support import ROOT, s

WITNESS = 'tests/selection/fixtures/groundhog_pre_gui_nonmembers_20261111.json'
SHA256 = '19cbc294a06b9af0b3c579c09e0b1e45d783a4e843414b626216f14984c4f5a4'


def historical_nonmember_policy():
    raw = (ROOT / WITNESS).read_bytes()
    s.require(hashlib.sha256(raw).hexdigest() == SHA256,
              'pre-GUI diagnostic nonmember witness changed')
    return json.loads(raw)['policy']
