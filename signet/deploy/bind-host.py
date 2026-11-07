#!/usr/bin/env python3
"""Bind an already installed release to approved public host identity. Never starts it."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import uuid
import jsonschema

p = argparse.ArgumentParser()
p.add_argument('bundle', type=Path)
p.add_argument('--volume-name', required=True)
p.add_argument('--config-revision', required=True)
p.add_argument('--profile', action='append', choices=['bootstrap', 'passkeys', 'ctap'], default=[])
a = p.parse_args()
if os.geteuid() != 0:
    raise SystemExit('requires root on the approved host')
bundle = a.bundle.resolve(strict=True)
if bundle.parent != Path('/opt/signet/releases') or not re.fullmatch('[0-9a-f]{40}', bundle.name):
    raise SystemExit('invalid installed bundle path')
if (bundle / 'manifest.json').exists():
    raise SystemExit('manifest already exists; do not erase upgrade/backup history')
m = json.loads((bundle / 'release.json').read_text())
config = Path('/opt/signet/config/signet.conf')
cfg = configparser.ConfigParser(interpolation=None)
with config.open() as f:
    cfg.read_file(f)
m.update(volume={'id': str(uuid.UUID(Path('/etc/signet-volume-id').read_text().strip())),
                 'filesystem_uuid': str(uuid.UUID(Path('/etc/signet-volume-uuid').read_text().strip())),
                 'name': a.volume_name}, profiles=a.profile,
         config={'revision': a.config_revision, 'sha256': hashlib.sha256(config.read_bytes()).hexdigest()},
         public_identifiers={'bunker': cfg.get('nostr', 'bunker_pubkey'),
                             'provisioners': [s.strip() for s in re.split('[,;]', cfg.get('nostr', 'provisioner_pubkeys', fallback='')) if s.strip()]},
         last_successful_backup=None, upgrade=None)
jsonschema.Draft202012Validator(json.loads((bundle / 'signet/deploy/manifest.schema.json').read_text()),
                               format_checker=jsonschema.FormatChecker()).validate(m)
variant = 'passkeys' if 'passkeys' in a.profile else 'default'
(bundle / 'release.env').write_text(f'SIGNET_IMAGE={m["images"][variant]}\nCADDY_IMAGE={m["images"]["caddy"]}\n')
with (bundle / 'manifest.json').open('x') as out:
    json.dump(m, out, indent=2)
    out.write('\n')
os.chmod(bundle / 'manifest.json', 0o600)
print('Manifest bound. Review it, pull selected digests, and run preflight before activation.')
