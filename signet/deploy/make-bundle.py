#!/usr/bin/env python3
"""Create a non-secret release bundle from the checked-out source and published digests."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--default-image', required=True)
p.add_argument('--passkeys-image', required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
for image in (a.default_image, a.passkeys_image):
    if not re.fullmatch(r'[a-z0-9./:_-]+@sha256:[0-9a-f]{64}', image):
        raise SystemExit('bundle requires published digests, not tags')
dirty = subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain',
                                 '--untracked-files=all', '--', 'signet/deploy', 'signet/docker-compose.yml'], text=True)
if dirty.strip():
    raise SystemExit('refusing to bind uncommitted deployment files to a source commit')
identity = json.loads(subprocess.check_output(['python3', str(root / 'signet/deploy/check-release.py')], text=True))
pins = dict(line.split('=', 1) for line in (root / 'signet/deploy/versions.env').read_text().splitlines() if line and not line.startswith('#'))
out = a.output / identity['sha']
out.mkdir(parents=True, exist_ok=False)
(out / 'signet').mkdir()
shutil.copyfile(root / 'signet/docker-compose.yml', out / 'signet/docker-compose.yml')
shutil.copytree(root / 'signet/deploy', out / 'signet/deploy',
                ignore=shutil.ignore_patterns('__pycache__', 'manifest.json', 'release.env', 'deploy.env', 'release.json'))
release = {'schema_version': 1, 'source_commit': identity['sha'], 'component_version': identity['version'],
           'images': {'default': a.default_image, 'passkeys': a.passkeys_image, 'caddy': pins['CADDY_IMAGE']},
           'packages': {'docker': pins['DOCKER_VERSION'], 'compose': pins['COMPOSE_VERSION'], 'containerd': pins['CONTAINERD_VERSION']}}
(out / 'release.json').write_text(json.dumps(release, indent=2) + '\n')
(out / 'release.env').write_text(f'SIGNET_IMAGE={a.default_image}\nCADDY_IMAGE={pins["CADDY_IMAGE"]}\n')
print(out)
