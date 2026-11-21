#!/usr/bin/env python3
"""Check fault catalog/source-hook inventory; the runtime mutation matrix is separate."""
import argparse
import json
import re
from pathlib import Path

core = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--check', action='store_true')
a = p.parse_args()
v = json.loads((core / 'tests/settle_fault_origins.json').read_text())
assert v['pin'] == '64f5b4d482cfe14abad108ffc2d92128f7658311'
names = re.findall(r'^    FIPS_SETTLE_FAULT_(\w+),$', (core / 'include/fips/proto/settle/faults.h').read_text(), re.M)
assert names == list(v['faults']) and len(names) == 147
hooks = {}
for path in (core / 'src/proto/settle').glob('*.c'):
    for name in re.findall(r'SETTLE_FAULT\((\w+)\)', path.read_text()):
        assert name in names, name
        hooks.setdefault(name, set()).add(path.name)

def boundary(name):
    if name in hooks:
        return 'ported core mutation'
    if name == 'IndexStale':
        return 'C derives its index directly from records; no cached index exists to leave stale'
    if name == 'AgedTieBreak':
        return 'alternative candidate-policy experiments; not production rules'
    if any('/' not in origin for origin in v['faults'][name]):
        return 'core mutation not yet ported'
    return 'guard of the unported world/joint/legacy/comparison infrastructure'

content = ['/* Generated inventory, not a coverage claim; see runtime mutation matrix. */',
           'static const struct {const char *name;bool supported;const char *boundary;} faults[]={']
for name in names:
    content.append('    {' + json.dumps(name) + ',' + ('true' if name in hooks else 'false') + ',' + json.dumps(boundary(name)) + '},')
content += ['};', '']
doc = ['# Settlement fault inventory', '', 'Pinned upstream: `' + v['pin'] + '`.', '',
       f'{len(names)} catalog names; {len(hooks)} have C mutation sites. Site presence is not',
       'a passing mutation result: `FipsSettleFaults` must kill every enabled mutation',
       'with an invariant assertion after each runner passes with none.', '',
       'No production fault selection, environment reads or mutable global state.',
       'Test threads explicitly borrow caller-owned scopes through a TLS pointer.',
       'Mutant resend scratch is bounded to64 outputs per poll; never used in production.',
       'Unsupported names refuse activation, rather than silently doing nothing.', '',
       '| Fault | C site / explicit boundary | Upstream sites |', '|---|---|---|']
for name in names:
    where = ', '.join(sorted(hooks[name])) if name in hooks else boundary(name)
    doc.append('| `' + name + '` | ' + where + ' | ' + ', '.join('`' + s + '`' for s in v['faults'][name]) + ' |')
doc += ['', 'The three candidate-policy alternatives are not enabled by this API.',
        'They need the retained-session test-shell protocol, not a production config knob.', '']
for path, text in [(core / 'src/proto/settle/faults_data.inc', '\n'.join(content)), (core / 'tests/SETTLE-FAULTS.md', '\n'.join(doc))]:
    if a.check:
        assert path.read_text() == text, f'stale inventory: {path}'
    else:
        path.write_text(text)
print(f'{len(names)} names, {len(hooks)} C mutation sites')
