#!/usr/bin/env python3
"""Transcribe immutable schedule DATA from the pinned Rust oracle (no algorithms)."""
import argparse
import json
from pathlib import Path

core = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--vectors', type=Path, default=core / 'tests/vectors/settle.json')
p.add_argument('--output', type=Path, default=core / 'src/proto/settle/parity_data.inc')
p.add_argument('--check', action='store_true')
a = p.parse_args()
v = json.loads(a.vectors.read_text())
assert v['pin'] == '64f5b4d482cfe14abad108ffc2d92128f7658311'
parity, = (c for c in v['cases'] if c['kind'] == 'parity')
q = lambda x: json.dumps(x, ensure_ascii=True)
b = lambda x: 'true' if x else 'false'
t = lambda x: {'udp': 'FIPS_PARITY_UDP', 'tcp': 'FIPS_PARITY_TCP', 'ble': 'FIPS_PARITY_BLE'}[x]
lines = ['/* Generated DATA by tests/tools/generate_settle_parity.py from',
         ' * upstream 64f5b4d482cfe14abad108ffc2d92128f7658311. Do not hand-edit. */']
for i, s in enumerate(parity['scripts']):
    assert len(s['name']) < 96 and len(s['settings']) <= 8
    lines.append(f'static const bool settings_{i}[]={{' + ','.join(map(b, s['settings'])) + '};')
    lines.append(f'static const fips_parity_transport transports_{i}[]={{' + ','.join(map(t, s['transports'])) + '};')
    lines.append(f'static const fips_parity_step steps_{i}[]={{')
    for step in s['steps']:
        kind, *x = step
        fields = {'kind': 'FIPS_PARITY_' + kind.upper()}
        if kind == 'Dial': fields.update(from_=str(x[0]), to=str(x[1]), transport=t(x[2]))
        elif kind in ('Connected', 'Restart', 'Rekey'): fields['to'] = str(x[0])
        elif kind == 'Handshake': fields.update(to=str(x[0]), msg2=b(x[1] == 'Msg2'), nth=str(x[2]))
        elif kind == 'Advance': fields['millis'] = f'UINT64_C({x[0]})'
        else: assert kind == 'Heal'
        lines.append('    {' + ','.join('.' + k.removesuffix('_') + '=' + val for k, val in fields.items()) + '},')
    lines.append('};')
lines.append('static const fips_parity_script scripts[]={')
for i, s in enumerate(parity['scripts']):
    lines.append('    {' + f'{q(s["name"])},settings_{i},{len(s["settings"])},{s["smaller"]},transports_{i},{len(s["transports"])},steps_{i},{len(s["steps"])}' + '},')
lines += ['};', f'_Static_assert(sizeof(scripts)/sizeof(*scripts)=={len(parity["scripts"])},"pinned script count");', 'static const char *const unshared[][2]={']
for name, why in parity['unshared']:
    lines.append('    {' + q(name) + ',' + q(why) + '},')
lines += ['};', '']
content = '\n'.join(lines)
if a.check:
    assert a.output.read_text() == content, f'stale parity data: {a.output}'
else:
    a.output.write_text(content)
print(f'{len(parity["scripts"])} scripts, {len(parity["unshared"])} unshared boundaries: {a.output}')
