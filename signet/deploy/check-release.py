#!/usr/bin/env python3
"""Check authoritative versions, pins and time-limited vulnerability exceptions."""
import argparse
import datetime as dt
import json
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--tag', default='')
p.add_argument('--ignore-output')
a = p.parse_args()
sources = [('signet/meson.build', r"version:\s*'([0-9]+\.[0-9]+\.[0-9]+)'"),
           ('signet/src/signetd_main.c', r'#define SIGNET_VERSION\s+"([^"]+)"'),
           ('signet/src/signetctl_main.c', r'#define SIGNETCTL_VERSION\s+"([^"]+)"')]
versions = []
for name, pattern in sources:
    match = re.search(pattern, (root / name).read_text())
    if not match:
        raise SystemExit(f'version not found: {name}')
    versions.append(match[1])
row = next(line for line in (root / 'VERSION_MANIFEST.md').read_text().splitlines()
           if re.search(r'\|\s*`?signet/?`?\s*\|', line, re.I))
versions.append(row.split('|')[3].strip().strip('`'))
if len(set(versions)) != 1:
    raise SystemExit(f'version disagreement: {versions}')
if a.tag and not re.fullmatch(r'signet-v' + re.escape(versions[0]) + r'(?:-[0-9A-Za-z.-]+)?', a.tag):
    raise SystemExit('tag disagrees with source base version')
pins = dict(line.split('=', 1) for line in (root / 'signet/deploy/versions.env').read_text().splitlines()
            if line and not line.startswith('#'))
for key in ('DEBIAN_IMAGE', 'CADDY_IMAGE'):
    if not re.fullmatch(r'[a-z0-9./:_-]+@sha256:[0-9a-f]{64}', pins[key]):
        raise SystemExit(f'unpinned {key}')
for key in ('CMAKE_VERSION', 'MESON_VERSION'):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', pins[key]):
        raise SystemExit(f'floating tool {key}')
exceptions = json.loads((root / 'signet/deploy/vulnerability-exceptions.json').read_text())
ids = []
for e in exceptions:
    if not re.fullmatch(r'CVE-[0-9]{4}-[0-9]+', e['id']) or not e['finding'] or not e['reason']:
        raise SystemExit('invalid vulnerability exception')
    if dt.date.fromisoformat(e['expires']) <= dt.datetime.now(dt.timezone.utc).date():
        raise SystemExit(f'expired exception: {e["id"]}')
    ids.append(e['id'])
if a.ignore_output:
    Path(a.ignore_output).write_text('\n'.join(ids) + '\n')
sha = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
print(json.dumps({'sha': sha, 'version': versions[0], 'image_tag': a.tag.removeprefix('signet-v') if a.tag else 'sha-' + sha}))
