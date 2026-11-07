#!/usr/bin/env python3
"""Regenerate only Signet release routing; preserve unrelated inventory bytes.

This checkout has no tracked whole-inventory generator. The inventory is the
selector's policy input, not a CMake-generated artifact. This narrow updater is
the owner of Signet package metadata and is checked by packaging CI.
"""
import argparse
import json
from pathlib import Path
import re
import sys

p = argparse.ArgumentParser()
p.add_argument('--check', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
path = root / 'tests/selection/inventory.json'
text = path.read_text()
start = text.index('"release_components":')
match = re.search(r'    "signet": (\{)', text[start:])
if match is None:
    raise SystemExit('Signet release metadata missing')
offset = start + match.start(1)
old, length = json.JSONDecoder().raw_decode(text[offset:])
old.update(publisher='signet', package_jobs=['package.default', 'package.passkeys'])
replacement = json.dumps(old, indent=2).replace('\n', '\n    ')
updated = text[:offset] + replacement + text[offset + length:]
assert json.loads(updated)['release_components']['signet'] == old

# Reuse the repository's source-job accounting parser; records explicitly keep
# release/legacy execution outside attested selective-profile coverage.
sys.path.insert(0, str(root / 'tests/selection'))
from profile_contract_support import source_job_blocks
blocks = source_job_blocks(root)
policy = json.loads(updated)
for key, block in blocks.items():
    if not key.startswith(('.github/workflows/signet-ci.yml:', '.github/workflows/signet-release.yml:')):
        continue
    record = dict(policy['workflow_jobs'].get(key, {
        'profiles': [], 'coverage_gap': 'workflow_profile_union',
        'unsupported_reason': 'Signet container release packaging/publication runs in its dedicated workflow; not an attested selective-profile execution receipt.'}))
    record.update(block)
    section = updated.index('"workflow_jobs":')
    found = re.search(r'    ' + re.escape(json.dumps(key)) + r': (\{)', updated[section:])
    encoded = json.dumps(record, indent=2).replace('\n', '\n    ')
    if found:
        pos = section + found.start(1)
        _, size = json.JSONDecoder().raw_decode(updated[pos:])
        updated = updated[:pos] + encoded + updated[pos + size:]
    else:
        pos = updated.index('{', section) + 1
        updated = updated[:pos] + '\n    ' + json.dumps(key) + ': ' + encoded + ',' + updated[pos:]
rule = {'exact': '.dockerignore', 'effect': 'impact', 'nodes': ['signet'],
        'reason': 'Signet image context and secret exclusion boundary.'}
if not any(r.get('exact') == '.dockerignore' for r in policy['path_rules']):
    pos = updated.index('[', updated.index('"path_rules":')) + 1
    encoded = json.dumps(rule, indent=2).replace('\n', '\n    ')
    updated = updated[:pos] + '\n    ' + encoded + ',' + updated[pos:]
json.loads(updated)
if a.check:
    if updated != text:
        raise SystemExit('Signet package metadata is stale; run signet/deploy/update-inventory.py')
else:
    path.write_text(updated)
