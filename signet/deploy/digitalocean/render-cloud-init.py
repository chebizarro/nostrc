#!/usr/bin/env python3
"""Render public inputs and repository-owned units; never accepts credentials."""
import os
from pathlib import Path
import re
import shlex
import sys
import uuid
import yaml

here = Path(__file__).resolve().parent
cfg = yaml.safe_load((here / 'cloud-init.yaml').read_text())
key = Path(os.environ['OPERATOR_SSH_PUBLIC_KEY_FILE']).read_text().strip()
if not re.fullmatch(r'(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp256) [A-Za-z0-9+/=]+(?: [^\n]*)?', key):
    raise SystemExit('invalid public SSH key')
volume = os.environ['DO_VOLUME_NAME']
if not re.fullmatch('[a-z0-9-]+', volume):
    raise SystemExit('invalid volume name')
volume_id = str(uuid.UUID(os.environ['DO_VOLUME_ID']))
pins = dict(line.split('=', 1) for line in (here.parent / 'versions.env').read_text().splitlines()
            if line and not line.startswith('#'))
cfg['users'][0]['ssh_authorized_keys'] = [key]
files = cfg['write_files']
for unit in (here.parent / 'systemd').iterdir():
    files.append({'path': '/etc/systemd/system/' + unit.name, 'owner': 'root:root',
                  'permissions': '0644', 'content': unit.read_text()})
files.append({'path': '/etc/signet-volume-id', 'owner': 'root:root', 'permissions': '0600', 'content': volume_id + '\n'})
script = '''#!/bin/bash
set -euo pipefail
install -d -m 0755 /etc/apt/keyrings
curl --fail --silent --show-error https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
test "$(gpg --show-keys --with-colons /etc/apt/keyrings/docker.asc | awk -F: '$1=="fpr" {print $10; exit}')" = 9DC858229FC7DD38854AE2D88D81803C0EBFCD88
chmod 0644 /etc/apt/keyrings/docker.asc
printf '%s\n' 'deb [arch=amd64 signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu noble stable' > /etc/apt/sources.list.d/docker.list
apt-get update
apt-get install -y --no-install-recommends docker-ce=DOCKER_PIN docker-ce-cli=DOCKER_PIN containerd.io=CONTAINERD_PIN docker-compose-plugin=COMPOSE_PIN
apt-mark hold docker-ce docker-ce-cli containerd.io docker-compose-plugin
# do-agent is installed by --enable-monitoring. Its absence is visible in the owner check.
getent group 10001 >/dev/null || groupadd -g 10001 signet
getent passwd 10001 >/dev/null || useradd -u 10001 -g 10001 -M -s /usr/sbin/nologin signet
test "$(id -u signet)" = 10001 && test "$(id -g signet)" = 10001
install -d -m 0755 /opt/signet /opt/signet/releases /srv/signet
install -d -m 0750 -o 10001 -g 10001 /opt/signet/config
install -d -m 0700 /opt/signet/secrets
# doctl creates a formatted ext4 volume. Never format an attached device here.
device=/dev/disk/by-id/scsi-0DO_Volume_VOLUME_NAME
udevadm settle --timeout=120
test -b "$device"
test "$(blkid -s TYPE -o value "$device")" = ext4
uuid=$(blkid -s UUID -o value "$device")
test -n "$uuid"
printf 'UUID=%s /srv/signet ext4 defaults,nodev,nosuid 0 2\n' "$uuid" >> /etc/fstab
mount /srv/signet
test "$(findmnt -n -o UUID --mountpoint /srv/signet)" = "$uuid"
install -d -m 0700 -o 10001 -g 10001 /srv/signet/state /srv/signet/backups
printf '%s\n' "$uuid" > /etc/signet-volume-uuid
chmod 0600 /etc/signet-volume-uuid
systemctl daemon-reload
# Deliberately disabled until keys, manifest and pinned images are installed.
systemctl disable signet-compose.service signet-backup.timer
'''
for token, value in [('DOCKER_PIN', pins['DOCKER_VERSION']), ('COMPOSE_PIN', pins['COMPOSE_VERSION']),
                     ('CONTAINERD_PIN', pins['CONTAINERD_VERSION']), ('VOLUME_NAME', volume)]:
    script = script.replace(token, shlex.quote(value))
files.append({'path': '/root/signet-host-init.sh', 'owner': 'root:root', 'permissions': '0700', 'content': script})
sys.stdout.write('#cloud-config\n' + yaml.safe_dump(cfg, sort_keys=False))
